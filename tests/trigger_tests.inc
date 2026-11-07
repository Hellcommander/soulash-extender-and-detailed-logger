// Included inside the anonymous namespace of tests.cpp (uses Check/failures).
// Tests for the Triggers plugin's pure parts: hook-point location
// (plugins/common/trigger_layout.hpp), the queued-ability deque push
// (plugins/common/ability_queue.hpp), the game-state reader
// (plugins/common/game_state.hpp) and rule evaluation
// (plugins/common/trigger_rules.hpp).

// ---- synthetic game image with the byte sequences of build 526b6ab4 ---------
struct TriggerImage
{
    static constexpr std::size_t Size = 0x4000;
    static constexpr std::size_t UseAbility = 0x1000;
    static constexpr std::size_t Attack = 0x1800;
    static constexpr std::size_t Queued = 0x1C00;
    static constexpr std::size_t OnCast = 0x2000;
    static constexpr std::size_t OnAttack = 0x2800;
    static constexpr std::size_t Registry = 0x3000;
    static constexpr std::size_t MapAt = 0x3100;
    static constexpr std::size_t New = 0x3200;
    static constexpr std::size_t GrowMap = 0x3300;
    static constexpr std::size_t Dtor = 0x3400;
    static constexpr std::size_t Delete = 0x3500;
    // Event hook anchors and targets.
    static constexpr std::size_t OnDeath = 0x3600;
    static constexpr std::size_t Kill = 0x3700;
    static constexpr std::size_t Progress = 0x3800;
    static constexpr std::size_t Tick = 0x3900;
    static constexpr std::size_t Clock = 0x3980;
    static constexpr std::size_t Parry = 0x3A00;
    static constexpr std::size_t Damage = 0x3A80;
    static constexpr std::size_t AttackEnd = 0x3B00;
    static constexpr std::size_t AttackUpdate = 0x3B80;
    static constexpr std::size_t Deflect = 0x3C00;
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(Size, 0xCC);

    std::size_t Put(std::size_t at, std::initializer_list<std::uint8_t> code)
    {
        std::copy(code.begin(), code.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
        return at + code.size();
    }

    std::size_t Rel(std::size_t at, std::size_t target)
    {
        const auto value = static_cast<std::int32_t>(
            static_cast<std::int64_t>(target) - static_cast<std::int64_t>(at + 4));
        std::memcpy(bytes.data() + at, &value, sizeof(value));
        return at + 4;
    }

    TriggerImage()
    {
        Put(0, {'M', 'Z'});
        const std::int32_t peOffset = 0x80;
        std::memcpy(bytes.data() + 0x3C, &peOffset, sizeof(peOffset));
        Put(0x80, {'P', 'E', 0, 0});
        const std::uint16_t magic = 0x20B;
        std::memcpy(bytes.data() + 0x80 + 24, &magic, sizeof(magic));
        const std::uint32_t sizeOfImage = static_cast<std::uint32_t>(Size);
        std::memcpy(bytes.data() + 0x80 + 24 + 56, &sizeOfImage, sizeof(sizeOfImage));

        // UseAbility::update: ... cmp byte [rax+258h],0; jne; lea r8,[rbp-50h]; mov edx,r14d; mov rcx,[r12+8]; call OnCast
        std::size_t at = UseAbility + 0x40;
        at = Put(at, {0x80, 0xB8, 0x58, 0x02, 0x00, 0x00, 0x00, 0x75, 0x11, 0x4C, 0x8D, 0x45, 0xB0, 0x41, 0x8B, 0xD6,
            0x49, 0x8B, 0x4C, 0x24, 0x08, 0xE8});
        at = Rel(at, OnCast);
        Put(at, {0xC3});

        // TriggerOnCast body fragments in the engine's order.
        at = OnCast;
        at = Put(at, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56});
        at = Put(at, {0xC7, 0x44, 0x24, 0x28, 0x09, 0x00, 0x00, 0x00, 0xC7, 0x44, 0x24, 0x20, 0x75, 0x00, 0x00, 0x00});
        at = Put(at, {0x83, 0x3F, 0x75, 0x0F, 0x85, 0x27, 0x02, 0x00, 0x00});
        at = Put(at, {0xE8});
        at = Rel(at, Registry);
        at = Put(at, {0x48, 0x8B, 0xC8, 0x4C, 0x8D, 0x47, 0x40, 0x48, 0x8D, 0x54, 0x24, 0x60, 0xE8});
        at = Rel(at, MapAt);
        at = Put(at, {0x48, 0x8B, 0xD8, 0xB9, 0x98, 0x01, 0x00, 0x00, 0xE8});
        at = Rel(at, New);
        at = Put(at, {0x48, 0x39, 0x8E, 0x40, 0x02, 0x00, 0x00, 0x77, 0x08, 0x48, 0x8B, 0xCD, 0xE8});
        at = Rel(at, GrowMap);
        at = Put(at, {0xB9, 0x10, 0x00, 0x00, 0x00, 0xE8});
        at = Rel(at, New);
        at = Put(at, {0x4E, 0x89, 0x24, 0xF1, 0x48, 0xFF, 0x86, 0x50, 0x02, 0x00, 0x00});
        Put(at, {0xC3});

        // EntityAttack (attack resolution): the parry-experience call on the miss
        // path, the damage-dealt call, the on-attack trigger call, the final call.
        at = Attack + 0x20;
        at = Put(at, {0x45, 0x8B, 0x45, 0x00, 0x8B, 0x16, 0x49, 0x8B, 0x4F, 0x08, 0xE8});
        at = Rel(at, Parry);
        Put(at, {0xC3});
        at = Attack + 0x40;
        at = Put(at, {0x44, 0x8B, 0x4D, 0x60, 0x45, 0x8B, 0x45, 0x00, 0x8B, 0x16, 0x49, 0x8B, 0x4F, 0x08, 0xE8});
        at = Rel(at, Damage);
        Put(at, {0xC3});
        // mov r9d,[rsi+60h]; mov r8d,[r13]; mov edx,[rsi]; mov rcx,[r15+8]; call OnAttack
        at = Attack + 0x60;
        at = Put(at, {0x44, 0x8B, 0x4E, 0x60, 0x45, 0x8B, 0x45, 0x00, 0x8B, 0x16, 0x49, 0x8B, 0x4F, 0x08, 0xE8});
        at = Rel(at, OnAttack);
        Put(at, {0xC3});
        at = Attack + 0x80;
        at = Put(at, {0x83, 0x7D, 0x64, 0x00, 0x41, 0x0F, 0x9F, 0xC1, 0x44, 0x8B, 0xC7, 0x48, 0x8B, 0xD6, 0x49, 0x8B, 0xCF, 0xE8});
        at = Rel(at, AttackEnd);
        Put(at, {0xC3});

        // OnDeath::update: the kill passives, effect 101 then 102, same routine.
        at = OnDeath + 0x20;
        at = Put(at, {0xC7, 0x44, 0x24, 0x20, 0x65, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x4D, 0xD8, 0x45, 0x8B, 0xC6, 0x41, 0x8B, 0xD7,
            0x48, 0x8B, 0xCE, 0xE8});
        at = Rel(at, Kill);
        at = Put(at, {0x41, 0xB8, 0x33, 0x00, 0x00, 0x00});
        at = Put(at, {0xC7, 0x44, 0x24, 0x20, 0x66, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x4D, 0xD8, 0x45, 0x8B, 0xC6, 0x41, 0x8B, 0xD7,
            0x48, 0x8B, 0xCE, 0xE8});
        at = Rel(at, Kill);
        Put(at, {0xC3});
        at = Kill;
        at = Put(at, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x41, 0xB8, 0x2C, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x15, 0x00, 0x00, 0x00});
        Put(at, {0xC3});

        // ProgressEffects::update: clock read, then the three per-turn calls.
        at = Progress + 0x10;
        at = Put(at, {0xE8});
        at = Rel(at, Clock);
        at = Put(at, {0xF3, 0x0F, 0x10, 0x40, 0x18, 0xF3, 0x0F, 0x5C, 0xC7, 0xF3, 0x0F, 0x5F, 0x45, 0x6C});
        at = Put(at, {0x4C, 0x8B, 0xC5, 0x41, 0x8B, 0xD7, 0x49, 0x8B, 0xCD, 0xE8});
        at = Rel(at, Dtor);
        at = Put(at, {0x41, 0x8B, 0xD7, 0x49, 0x8B, 0xCD, 0xE8});
        at = Rel(at, Tick);
        at = Put(at, {0x4C, 0x8B, 0xC5, 0x41, 0x8B, 0xD7, 0x49, 0x8B, 0xCD, 0xE8});
        at = Rel(at, Dtor);
        Put(at, {0xC3});
        at = Tick;
        at = Put(at, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0xC7, 0x44, 0x24, 0x28, 0x09, 0x00, 0x00, 0x00,
            0xC7, 0x44, 0x24, 0x20, 0x3F, 0x00, 0x00, 0x00});
        Put(at, {0xC3});
        Put(Clock, {0x48, 0x83, 0xEC, 0x28, 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00, 0xC3});

        // Parry experience and damage dealt both start with hasComponent(target, 3).
        at = Parry;
        at = Put(at, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x8B, 0xD3, 0xE8});
        at = Rel(at, Delete);
        Put(at, {0xC3});
        at = Damage;
        at = Put(at, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x41, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x8B, 0xD3, 0xE8});
        at = Rel(at, Delete);
        Put(at, {0xC3});
        Put(AttackEnd, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x41, 0xB9, 0x1E, 0x00, 0x00, 0x00, 0xC3});

        // EntityAttack::update: call Deflect; movzx ebx,al; test al,al; je; lea rdx,[rbp+...]
        at = AttackUpdate + 0x10;
        at = Put(at, {0xE8});
        at = Rel(at, Deflect);
        at = Put(at, {0x0F, 0xB6, 0xD8, 0x84, 0xC0, 0x74, 0x54, 0x48, 0x8D, 0x95});
        Put(at, {0xC3});
        at = Deflect;
        at = Put(at, {0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x57, 0x49, 0x8B, 0x48, 0x20, 0x48, 0xFF, 0xC9, 0x49, 0x23, 0x48, 0x28,
            0x49, 0x8B, 0x40, 0x18, 0x48, 0x8B, 0x0C, 0xC8, 0x8B, 0x59, 0x4C});
        Put(at, {0xC3});
        at = OnAttack;
        at = Put(at, {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});
        at = Put(at, {0x41, 0xB8, 0x4C, 0x00, 0x00, 0x00, 0x8B, 0xD6, 0x48, 0x8B, 0xCB});
        at = Put(at, {0xC7, 0x44, 0x24, 0x28, 0x09, 0x00, 0x00, 0x00, 0xC7, 0x44, 0x24, 0x20, 0x64, 0x00, 0x00, 0x00});
        Put(at, {0xC3});

        // Queued Ability: cmp [r8+250h],0 ... call ~Ability; mov edx,198h; mov rcx,rbx; call delete
        at = Queued;
        at = Put(at, {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x49, 0x83, 0xB8, 0x50, 0x02, 0x00, 0x00, 0x00, 0x75, 0x07});
        at = Put(at, {0x48, 0x8B, 0xCB, 0xE8});
        at = Rel(at, Dtor);
        at = Put(at, {0xBA, 0x98, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0xE8});
        at = Rel(at, Delete);
        Put(at, {0xC3});
        for (std::size_t stub : {Registry, MapAt, New, GrowMap, Dtor, Delete})
        {
            Put(stub, {0xC3});
        }
    }

    const std::uint8_t* At(std::size_t offset) const { return bytes.data() + offset; }

    soulash_plugins::TriggerLayout Locate(bool withAttack = true, bool withQueued = true) const
    {
        return soulash_plugins::LocateTriggerLayout(bytes.data(), bytes.size(), At(UseAbility),
            withAttack ? At(Attack) : nullptr, withQueued ? At(Queued) : nullptr);
    }

    soulash_plugins::TriggerAnchors Anchors() const
    {
        soulash_plugins::TriggerAnchors anchors;
        anchors.useAbilityUpdate = At(UseAbility);
        anchors.entityAttack = At(Attack);
        anchors.queuedAbility = At(Queued);
        anchors.onDeathUpdate = At(OnDeath);
        anchors.entityAttackUpdate = At(AttackUpdate);
        anchors.parryExperience = At(Parry);
        anchors.progressEffectsUpdate = At(Progress);
        return anchors;
    }

    soulash_plugins::TriggerLayout LocateAll() const
    {
        return soulash_plugins::LocateTriggerLayout(bytes.data(), bytes.size(), Anchors());
    }
};

void TestTriggerLayout()
{
    {
        TriggerImage image;
        const auto layout = image.Locate();
        Check(layout.verified, "trigger layout: synthetic image verifies");
        Check(layout.failure[0] == '\0', "trigger layout: no note when everything is found");
        Check(layout.onCastTrigger == image.At(TriggerImage::OnCast), "trigger layout: on-cast routine from the call site");
        Check(layout.onAttackTrigger == image.At(TriggerImage::OnAttack), "trigger layout: on-attack routine");
        Check(layout.abilityRegistry == image.At(TriggerImage::Registry), "trigger layout: ability registry getter");
        Check(layout.abilityMapAt == image.At(TriggerImage::MapAt), "trigger layout: map::at");
        Check(layout.gameNew == image.At(TriggerImage::New), "trigger layout: operator new");
        Check(layout.dequeGrowMap == image.At(TriggerImage::GrowMap), "trigger layout: deque growmap");
        Check(layout.abilityDestructor == image.At(TriggerImage::Dtor), "trigger layout: ~Ability");
        Check(layout.gameDelete == image.At(TriggerImage::Delete), "trigger layout: operator delete");
    }
    {
        TriggerImage image;
        const auto layout = image.Locate(false, false);
        Check(layout.verified && layout.onCastTrigger != nullptr, "trigger layout: cast hook alone still verifies");
        Check(layout.onAttackTrigger == nullptr && layout.abilityDestructor == nullptr,
            "trigger layout: missing anchors leave their fields null");
        Check(layout.failure[0] != '\0', "trigger layout: a note names the missing attack routine");
    }
    {
        TriggerImage image;
        image.bytes[TriggerImage::OnCast + 0x1C] = 0x76;   // effect 117 -> 118
        Check(!image.Locate().verified, "trigger layout: a different passive effect id is rejected");
    }
    {
        TriggerImage image;
        image.bytes[TriggerImage::UseAbility + 0x42] = 0x59;   // flag offset 0x258 -> 0x259
        Check(!image.Locate().verified, "trigger layout: a changed call site is rejected");
    }
    {
        TriggerImage image;
        // Block allocation through a different function than the Ability allocation.
        const std::size_t blockCall = TriggerImage::OnCast + 0x10 + 0x10 + 0x09 + 0x05 + 0x11 + 0x09 + 0x11 + 0x06;
        image.Rel(blockCall, TriggerImage::GrowMap);
        Check(!image.Locate().verified, "trigger layout: two different allocators are rejected");
    }
    {
        TriggerImage image;
        Check(!soulash_plugins::LocateTriggerLayout(image.bytes.data(), image.bytes.size(), nullptr, nullptr, nullptr).verified,
            "trigger layout: no UseAbility function -> not verified");
    }
    // ---- event hooks ----------------------------------------------------------
    {
        TriggerImage image;
        const auto layout = image.LocateAll();
        Check(layout.verified && layout.failure[0] == '\0', "event hooks: the full anchor set still verifies");
        Check(layout.onKillTrigger == image.At(TriggerImage::Kill), "event hooks: kill routine from OnDeath::update");
        Check(layout.onTurnTick == image.At(TriggerImage::Tick), "event hooks: turn tick from ProgressEffects::update");
        Check(layout.gameClock == image.At(TriggerImage::Clock), "event hooks: game clock getter");
        Check(layout.onParry == image.At(TriggerImage::Parry), "event hooks: parry routine (dodge)");
        Check(layout.onDamageDealt == image.At(TriggerImage::Damage), "event hooks: damage-dealt routine");
        Check(layout.onAttackEnd == image.At(TriggerImage::AttackEnd), "event hooks: attack-end routine (crit)");
        Check(layout.onDeflect == image.At(TriggerImage::Deflect), "event hooks: deflection routine (block)");
        const auto core = image.Locate();
        Check(core.onKillTrigger == nullptr && core.onTurnTick == nullptr && core.onParry == nullptr
                && core.onDeflect == nullptr && core.gameClock == nullptr,
            "event hooks: without their anchors the fields stay null");
    }
    {
        TriggerImage image;
        image.Rel(TriggerImage::OnDeath + 0x20 + 22 + 4 + 6 + 22, TriggerImage::Dtor);   // 102 site calls elsewhere
        Check(image.LocateAll().onKillTrigger == nullptr, "event hooks: the two kill sites must agree");
    }
    {
        TriggerImage image;
        image.bytes[TriggerImage::Tick + 0x13] = 0x40;   // effect 63 -> 64
        const auto layout = image.LocateAll();
        Check(layout.onTurnTick == nullptr && layout.gameClock == image.At(TriggerImage::Clock),
            "event hooks: a tick routine with another effect id is rejected, the clock is independent");
    }
    {
        TriggerImage image;
        image.bytes[TriggerImage::Parry + 7] = 0x04;   // hasComponent(target, 4)
        Check(image.LocateAll().onParry == nullptr, "event hooks: parry anchor without the Actor check is rejected");
    }
    {
        TriggerImage image;
        image.bytes[TriggerImage::Deflect + 7 + 20] = 0x48;   // entry.target offset 0x4C -> 0x48
        Check(image.LocateAll().onDeflect == nullptr, "event hooks: deflection with another entry layout is rejected");
    }
}

// ---- deque push against a real std::deque<void*> ---------------------------
namespace trigger_test_detail
{
    // MSVC's _Growmap(1) with the CRT allocator, so the deque's own destructor
    // can free what we allocated. Maps stay under 4 KiB in these tests: above
    // that MSVC's allocator over-aligns and the destructor would expect that
    // layout (the game's own _Growmap handles it; this substitute does not).
    void GrowMapLikeMsvc(soulash_plugins::DequeOfPointers& d)
    {
        std::size_t newSize = d.mapSize > 0 ? d.mapSize : 1;
        while (newSize - d.mapSize < 1 || newSize < 8) newSize *= 2;
        const std::size_t blockOffset = d.offset / soulash_plugins::DequeBlockElements;
        void*** newMap = static_cast<void***>(::operator new(newSize * sizeof(void*)));
        std::memset(newMap, 0, newSize * sizeof(void*));
        if (d.map != nullptr)
        {
            // Blocks [blockOffset, mapSize) keep their index; [0, blockOffset) move up.
            for (std::size_t i = blockOffset; i < d.mapSize; ++i) newMap[i] = d.map[i];
            for (std::size_t i = 0; i < blockOffset; ++i) newMap[d.mapSize + i] = d.map[i];
            ::operator delete(d.map, d.mapSize * sizeof(void*));
        }
        d.map = newMap;
        d.mapSize = newSize;
    }
}

void TestAbilityQueue()
{
    using soulash_plugins::DequeOfPointers;
    static_assert(sizeof(std::deque<void*>) == sizeof(DequeOfPointers), "std::deque layout assumption");
    std::vector<int> cells(2000);
    auto push = [](std::deque<void*>& d, void* value) {
        return soulash_plugins::DequePushBack(*reinterpret_cast<DequeOfPointers*>(&d), value,
            [](std::size_t bytes) { return ::operator new(bytes); }, trigger_test_detail::GrowMapLikeMsvc);
    };
    {
        std::deque<void*> d;
        for (int i = 0; i < 400; ++i) Check(push(d, &cells[i]), "queue: push succeeds");
        Check(d.size() == 400, "queue: 400 pushes give size 400");
        bool ordered = true;
        for (int i = 0; i < 400; ++i) ordered = ordered && d[static_cast<std::size_t>(i)] == &cells[i];
        Check(ordered, "queue: elements come back in push order through std::deque");
        Check(d.front() == &cells[0] && d.back() == &cells[399], "queue: front/back agree");
    }
    {
        // Interleave with the real container, including a wrapped-around offset.
        std::deque<void*> d;
        for (int i = 0; i < 7; ++i) d.push_back(&cells[i]);
        for (int i = 0; i < 5; ++i) d.pop_front();
        for (int i = 7; i < 40; ++i) Check(push(d, &cells[i]), "queue: push after pops succeeds");
        std::vector<void*> expected;
        for (int i = 5; i < 40; ++i) expected.push_back(&cells[i]);
        Check(std::vector<void*>(d.begin(), d.end()) == expected, "queue: mixed std/our pushes keep order");
        for (int i = 0; i < 30; ++i) d.pop_front();
        d.push_back(&cells[1000]);
        Check(d.size() == 6 && d.back() == &cells[1000], "queue: the container keeps working afterwards");
    }
    {
        std::deque<void*> d;
        bool failed = soulash_plugins::DequePushBack(*reinterpret_cast<DequeOfPointers*>(&d), &cells[0],
            [](std::size_t) -> void* { return nullptr; }, trigger_test_detail::GrowMapLikeMsvc);
        Check(!failed && d.empty(), "queue: a failed block allocation leaves the deque empty");
    }
}

// ---- game-state reader over fake component memory --------------------------
namespace trigger_test_detail
{
    namespace gs = soulash_plugins::game_state;

    // One ECStorage<T> with a single page and a dense array.
    struct FakeStorage
    {
        std::vector<std::uint8_t> object = std::vector<std::uint8_t>(0x48, 0);
        std::vector<std::int32_t> page = std::vector<std::int32_t>(0x2000, -1);
        std::vector<std::uint8_t> pageEntry = std::vector<std::uint8_t>(24, 0);
        std::vector<std::uint8_t> dense;
        std::vector<std::int32_t> ids;
        std::size_t stride;
        std::uint8_t vtable[8]{};

        explicit FakeStorage(std::size_t strideBytes) : stride(strideBytes)
        {
            const std::int32_t* begin = page.data();
            const std::int32_t* end = page.data() + page.size();
            std::memcpy(pageEntry.data(), &begin, 8);
            std::memcpy(pageEntry.data() + 8, &end, 8);
            std::memcpy(pageEntry.data() + 16, &end, 8);
            const std::uint8_t* pb = pageEntry.data();
            const std::uint8_t* pe = pageEntry.data() + 24;
            std::memcpy(object.data() + gs::StoragePagesBeginOffset, &pb, 8);
            std::memcpy(object.data() + gs::StoragePagesEndOffset, &pe, 8);
        }

        // Adds a component for `id`; returns its address (stable after Finish).
        std::size_t Add(std::uint32_t id)
        {
            page[id & gs::StoragePageMask] = static_cast<std::int32_t>(dense.size() / stride);
            dense.resize(dense.size() + stride, 0);
            const void* vt = vtable;
            std::memcpy(dense.data() + dense.size() - stride, &vt, 8);
            ids.push_back(static_cast<std::int32_t>(id));
            return dense.size() - stride;
        }

        void Finish()
        {
            const std::uint8_t* db = dense.data();
            const std::uint8_t* de = dense.data() + dense.size();
            std::memcpy(object.data() + gs::StorageDenseBeginOffset, &db, 8);
            std::memcpy(object.data() + gs::StorageDenseEndOffset, &de, 8);
            const std::int32_t* ib = ids.data();
            const std::int32_t* ie = ids.data() + ids.size();
            std::memcpy(object.data() + gs::StorageIdsBeginOffset, &ib, 8);
            std::memcpy(object.data() + gs::StorageIdsEndOffset, &ie, 8);
        }

        gs::StorageRef Ref() const
        {
            gs::StorageRef ref;
            ref.storage = object.data();
            ref.stride = stride;
            ref.componentVtable = vtable;
            return ref;
        }

        template <typename T>
        void Set(std::size_t component, std::uintptr_t offset, T value)
        {
            std::memcpy(dense.data() + component + offset, &value, sizeof(T));
        }
    };

    // Writes an MSVC std::string (SSO or heap) into 32 bytes at `at`.
    void PutString(std::uint8_t* at, const std::string& text, std::vector<std::unique_ptr<std::string>>& keep)
    {
        auto owned = std::make_unique<std::string>(text);
        std::memcpy(at, owned.get(), 0x20);   // same STL: byte-identical layout
        keep.push_back(std::move(owned));
    }
}

void TestGameState()
{
    using namespace trigger_test_detail;
    FakeStorage health(gs::HealthSize), stamina(gs::StaminaSize), tags(gs::TagsSize), equipment(gs::EquipmentSize),
        skills(gs::SkillsSize), weapon(gs::WeaponSize), item(gs::ItemSize), abilityUser(gs::AbilityUserSize),
        effects(gs::EffectsSize), party(gs::PartySize), player(gs::PlayerSize), name(gs::NameSize),
        stackable(gs::StackableSize);
    const std::uint32_t holder = 4014, target = 77, sword = 100, shield = 101;   // one page: ids < 0x2000
    const std::uint32_t companionA = 300, companionB = 301, breadStack = 500, breadSingle = 501, nameless = 502;
    std::vector<std::unique_ptr<std::string>> keep;

    const std::size_t h = health.Add(holder);
    health.Set<float>(h, gs::HealthCurrentOffset, 52.5f);
    health.Set<float>(h, gs::HealthMaximumOffset, 105.0f);
    const std::size_t th = health.Add(target);
    health.Set<float>(th, gs::HealthCurrentOffset, 10.0f);
    health.Set<float>(th, gs::HealthMaximumOffset, 100.0f);
    health.Finish();

    const std::size_t s = stamina.Add(holder);
    stamina.Set<float>(s, gs::StaminaUsedOffset, 25.0f);
    stamina.Set<float>(s, gs::StaminaMaximumOffset, 100.0f);
    stamina.Finish();

    // Tags: vector<std::string> {"5","1","custom"} for the holder, {"4"} for the target.
    std::vector<std::uint8_t> holderTagStrings(3 * 0x20), targetTagStrings(0x20);
    PutString(holderTagStrings.data(), "5", keep);
    PutString(holderTagStrings.data() + 0x20, "1", keep);
    PutString(holderTagStrings.data() + 0x40, "custom", keep);
    PutString(targetTagStrings.data(), "4", keep);
    const std::size_t t = tags.Add(holder);
    tags.Set<const std::uint8_t*>(t, gs::TagsBeginOffset, holderTagStrings.data());
    tags.Set<const std::uint8_t*>(t, gs::TagsEndOffset, holderTagStrings.data() + holderTagStrings.size());
    const std::size_t tt = tags.Add(target);
    tags.Set<const std::uint8_t*>(tt, gs::TagsBeginOffset, targetTagStrings.data());
    tags.Set<const std::uint8_t*>(tt, gs::TagsEndOffset, targetTagStrings.data() + 0x20);
    std::vector<std::uint8_t> swordTagStrings(0x20);
    PutString(swordTagStrings.data(), "artifact", keep);
    const std::size_t st = tags.Add(sword);
    tags.Set<const std::uint8_t*>(st, gs::TagsBeginOffset, swordTagStrings.data());
    tags.Set<const std::uint8_t*>(st, gs::TagsEndOffset, swordTagStrings.data() + 0x20);
    tags.Finish();

    // Backpack: a stack of 3 bread, one single bread, one entity without a Name.
    std::vector<std::int32_t> backpack{static_cast<std::int32_t>(breadStack), static_cast<std::int32_t>(breadSingle),
        static_cast<std::int32_t>(nameless)};
    const std::size_t e = equipment.Add(holder);
    equipment.Set<std::int32_t>(e, gs::EquipmentSlotsOffset + 4 * gs::SlotRightHand, static_cast<std::int32_t>(sword));
    equipment.Set<std::int32_t>(e, gs::EquipmentSlotsOffset + 4 * gs::SlotLeftHand, static_cast<std::int32_t>(shield));
    equipment.Set<const std::int32_t*>(e, gs::EquipmentInventoryBeginOffset, backpack.data());
    equipment.Set<const std::int32_t*>(e, gs::EquipmentInventoryEndOffset, backpack.data() + backpack.size());
    equipment.Finish();

    // Name: data id at +8, display name at +0x28.
    for (const auto& [entity, id, display] : std::vector<std::tuple<std::uint32_t, const char*, const char*>>{
             {sword, "core_2_Ice_Rod", "Ice Rod"}, {shield, "183", "Robe"}, {breadStack, "803", "Wheat Bread"},
             {breadSingle, "803", "Wheat Bread"}})
    {
        const std::size_t n = name.Add(entity);
        PutString(name.dense.data() + n + gs::NameDataIdOffset, id, keep);
        PutString(name.dense.data() + n + gs::NameDisplayOffset, display, keep);
    }
    name.Finish();
    const std::size_t sc = stackable.Add(breadStack);
    stackable.Set<std::int32_t>(sc, gs::StackableCountOffset, 3);
    stackable.Finish();

    const std::size_t w = weapon.Add(sword);
    weapon.Set<std::int32_t>(w, gs::WeaponTypeOffset, 1);   // Sword
    weapon.Finish();
    const std::size_t i1 = item.Add(sword);
    item.Set<std::int32_t>(i1, gs::ItemTypeOffset, 13);     // Weapon
    const std::size_t i2 = item.Add(shield);
    item.Set<std::int32_t>(i2, gs::ItemTypeOffset, static_cast<std::int32_t>(gs::ItemTypeShield));
    item.Finish();

    // Skills: a real std::map<std::string,int> copied into the component.
    std::map<std::string, int> levels{{"arendeth_runic_magic", 5}, {"core_2_alchemy", 1}, {"zzz_last", 12}};
    const std::size_t sk = skills.Add(holder);
    std::memcpy(skills.dense.data() + sk + gs::SkillsLevelMapOffset, &levels, sizeof(levels));
    skills.Finish();

    const std::size_t au = abilityUser.Add(holder);
    abilityUser.Set<std::int32_t>(au, gs::AbilityUserTargetOffset, static_cast<std::int32_t>(target));
    abilityUser.Finish();

    // Effects: stacks {flag 3, burn 2, old 5, none 0}, expiry {burn 50, old 10}; the clock says 20.
    std::map<std::string, int> stacks{{"flag", 3}, {"burn", 2}, {"old", 5}, {"none", 0}};
    std::map<std::string, float> expiry{{"burn", 50.0f}, {"old", 10.0f}};
    const std::size_t ef = effects.Add(holder);
    std::memcpy(effects.dense.data() + ef + gs::EffectsStacksMapOffset, &stacks, sizeof(stacks));
    std::memcpy(effects.dense.data() + ef + gs::EffectsStackExpiryMapOffset, &expiry, sizeof(expiry));
    effects.Set<float>(ef, gs::EffectsLastTickOffset, 20.0f);
    effects.Finish();

    // Party: two companions carry the component; the holder is the player.
    party.Add(companionA);
    party.Add(companionB);
    party.Finish();
    player.Add(holder);
    player.Finish();

    std::vector<std::uint8_t> clockBytes(0x30, 0);
    const std::int32_t turnsPerDay = 1440, dayPart = 10, yearPart = 3;
    const float now = 20.0f;
    std::memcpy(clockBytes.data() + gs::ClockTurnsPerDayOffset, &turnsPerDay, 4);
    std::memcpy(clockBytes.data() + gs::ClockTimeOffset, &now, 4);
    std::memcpy(clockBytes.data() + gs::ClockDayPartOffset, &dayPart, 4);
    std::memcpy(clockBytes.data() + gs::ClockYearPartOffset, &yearPart, 4);
    const gs::GameClock clock = gs::ReadClock(clockBytes.data());
    Check(clock.has && clock.time == 20.0f && clock.dayPart == 10 && clock.yearPart == 3, "state: clock struct read");

    gs::Access access;
    access.health = health.Ref();
    access.stamina = stamina.Ref();
    access.tags = tags.Ref();
    access.equipment = equipment.Ref();
    access.skills = skills.Ref();
    access.weapon = weapon.Ref();
    access.item = item.Ref();
    access.abilityUser = abilityUser.Ref();
    access.effects = effects.Ref();
    access.party = party.Ref();
    access.player = player.Ref();
    access.name = name.Ref();
    access.stackable = stackable.Ref();

    soulash::conditions::Snapshot state;
    gs::BuildSnapshot(access, holder, target, state, clock);
    Check(state.equipped.has && state.equipped.value.at("right_hand").item == "core_2_Ice_Rod"
            && state.equipped.value.at("left_hand").item == "183"
            && state.equipped.value.at("right_hand").itemTags == std::set<std::string>{"artifact"}
            && state.equipped.value.at("left_hand").itemTags.empty(),
        "state: equipped items carry their data id and entity tags");
    Check(state.inventory.has && state.inventory.value.size() == 1 && state.inventory.value.at("803") == 4,
        "state: backpack counts stacks per data id and skips nameless entities");
    Check(state.stackers.has && state.stackers.value.size() == 2 && state.stackers.value.at("flag") == 3
            && state.stackers.value.at("burn") == 2,
        "state: live stacks, expired and zero stacks left out");
    Check(state.party.has && state.party.value == "in_party" && state.companions.has && state.companions.value == 2,
        "state: the player with two Party-component companions is in a party of two");
    Check(state.dayPart.has && state.dayPart.value == "Night" && state.season.has && state.season.value == "Winter",
        "state: day part and season names from the clock indices");
    {
        soulash::conditions::Snapshot companion;
        gs::BuildSnapshot(access, companionA, 0, companion, clock);
        Check(companion.party.has && companion.party.value == "in_party" && companion.companions.value == 2,
            "state: a companion is in the party with the other member and the player");
        soulash::conditions::Snapshot stranger;
        gs::BuildSnapshot(access, target, 0, stranger, clock);
        Check(stranger.party.has && stranger.party.value == "alone" && stranger.companions.value == 0,
            "state: an entity outside the party is alone");
        soulash::conditions::Snapshot noClock;
        gs::BuildSnapshot(access, holder, target, noClock);
        Check(!noClock.dayPart.has && !noClock.season.has && noClock.stackers.has && noClock.stackers.value.size() == 2,
            "state: without a clock the time predicates stay unknown and stacks use the last tick time");
    }
    Check(state.healthPercent.has && state.healthPercent.value == 50.0, "state: health percent");
    Check(state.staminaPercent.has && state.staminaPercent.value == 75.0, "state: stamina = (max - used) / max");
    Check(state.holderTags.has && state.holderTags.value == std::set<std::string>{"Player", "Human", "custom"},
        "state: tag indices become names, unknown ones stay as written");
    Check(state.targetTags.has && state.targetTags.value == std::set<std::string>{"Undead"}, "state: target tags");
    Check(state.targetHealthPercent.has && state.targetHealthPercent.value == 10.0, "state: target health");
    Check(state.weaponClass.has && state.weaponClass.value == "Sword", "state: weapon class from the right hand");
    Check(state.shield.has && state.shield.value, "state: shield in the left hand");
    Check(state.equipped.has && state.equipped.value.count("right_hand") == 1 && state.equipped.value.count("head_armor") == 0
            && state.equipped.value.at("left_hand").itemType == "Shield",
        "state: equipped slots carry the item type");
    Check(state.skills.has && state.skills.value.size() == 3 && state.skills.value.at("arendeth_runic_magic") == 5
            && state.skills.value.at("zzz_last") == 12,
        "state: skill levels read from the live std::map");
    Check(!state.biome.has && !state.inCombat.has && !state.inSettlement.has,
        "state: predicates without a backing stay unknown");
    {
        gs::Access noNames = access;
        noNames.name = gs::StorageRef{};
        soulash::conditions::Snapshot blind;
        gs::BuildSnapshot(noNames, holder, target, blind, clock);
        Check(!blind.inventory.has && blind.equipped.has && blind.equipped.value.at("right_hand").item.empty(),
            "state: without the Name storage the inventory is unknown and items keep no id");
    }

    // The evaluator sees the same picture.
    const std::string document = "{\"spec\":1,\"triggers\":[{\"id\":\"t\",\"on\":{\"cast\":\"a\"},\"cast\":\"b\","
        "\"when\":{\"all\":[{\"health_percent\":{\"max\":50}},{\"weapon_class\":\"sword\"},{\"shield\":true},"
        "{\"holder_tag\":\"player\"},{\"target_tag\":[\"undead\"]},{\"skill_level\":{\"skill\":\"arendeth_runic_magic\",\"min\":5}},"
        "{\"equipped\":{\"slot\":\"left_hand\",\"item_type\":\"Shield\"}},{\"stamina_percent\":{\"min\":70}},"
        "{\"stacker\":{\"id\":\"flag\",\"min\":3}},{\"stacker\":{\"id\":\"old\",\"max\":0}},{\"party\":\"in_party\"},"
        "{\"companions\":{\"min\":2,\"max\":2}},{\"time_of_day\":\"night\"},{\"season\":[\"winter\"]},"
        "{\"equipped\":{\"slot\":\"right_hand\",\"item\":\"core_2_Ice_Rod\"}},{\"equipped\":{\"slot\":\"right_hand\",\"item_tag\":\"artifact\"}},"
        "{\"inventory_count\":{\"item\":\"803\",\"min\":4,\"max\":4}},{\"inventory_count\":{\"item\":\"999\",\"max\":0}}]}}]}";
    soulash::conditions::ConditionSet set;
    std::string error;
    Check(set.Load(document, error), "state: evaluator document loads");
    Check(set.Triggers().size() == 1 && soulash::conditions::IsTriggerAllowed(set, set.Triggers()[0], state),
        "state: a trigger over every backed predicate is allowed on the snapshot");

    // Absent entity, wrong vtable, unknown holder.
    soulash::conditions::Snapshot nobody;
    gs::BuildSnapshot(access, 4242, 0, nobody);
    Check(!nobody.healthPercent.has && !nobody.equipped.has && !nobody.holderTags.has, "state: unknown entity -> all unknown");
    std::uint8_t otherVtable[8]{};
    access.health.componentVtable = otherVtable;   // the components still point at the old vtable
    soulash::conditions::Snapshot mismatched;
    gs::BuildSnapshot(access, holder, 0, mismatched);
    Check(!mismatched.healthPercent.has && mismatched.staminaPercent.has, "state: a vtable mismatch hides only that component");
}

// ---- rule evaluation --------------------------------------------------------
void TestTriggerRules()
{
    namespace rules = soulash_plugins::trigger_rules;
    std::vector<rules::ModRules> mods(1);
    mods[0].mod = "m";
    std::string error;
    Check(mods[0].conditions.Load(
              "{\"spec\":1,\"triggers\":["
              "{\"id\":\"wounded\",\"on\":{\"cast\":\"A\"},\"cast\":\"B\",\"when\":{\"health_percent\":{\"max\":50}}},"
              "{\"id\":\"lucky\",\"on\":{\"cast\":\"A\"},\"cast\":\"C\",\"chance\":0.5,\"when\":{\"holder_tag\":\"Player\"}},"
              "{\"id\":\"always\",\"on\":{\"cast\":\"A\"},\"cast\":\"D\",\"when\":{\"time_of_day\":\"Night\"},\"on_unavailable\":\"active\"},"
              "{\"id\":\"swing\",\"on\":{\"attack\":true},\"cast\":\"E\",\"when\":{\"weapon_class\":\"Axe\"}},"
              "{\"id\":\"vengeful\",\"on\":{\"damaged\":true},\"cast\":\"F\",\"when\":{\"health_percent\":{\"max\":50}}},"
              "{\"id\":\"reaper\",\"on\":{\"kill\":true},\"cast\":\"G\",\"when\":{\"holder_tag\":\"Player\"}},"
              "{\"id\":\"tick\",\"on\":{\"turn\":true},\"cast\":\"H\",\"when\":{\"stacker\":{\"id\":\"flag\",\"min\":1}}}]}",
              error),
        "rules: document loads");
    mods[0].hasConditions = true;

    soulash::conditions::Snapshot state;
    state.healthPercent = 30.0;
    state.holderTags = std::set<std::string>{"Player"};
    state.weaponClass = std::string("Axe");

    rules::Event cast{rules::EventKind::Cast, "A"};
    rules::Event other{rules::EventKind::Cast, "Z"};
    rules::Event attack{rules::EventKind::Attack, ""};
    Check(rules::AnyRuleFor(mods, cast) && !rules::AnyRuleFor(mods, other) && rules::AnyRuleFor(mods, attack),
        "rules: pre-check by event");

    auto low = [] { return 0.1; };
    auto high = [] { return 0.9; };
    rules::Outcome out = rules::Evaluate(mods, cast, state, low, 8);
    Check(out.matched == 3 && out.allowed == 3 && out.casts.size() == 3, "rules: all three cast rules fire on a low roll");
    Check(out.casts[0].abilityId == "B" && out.casts[1].abilityId == "C" && out.casts[2].abilityId == "D"
            && out.casts[1].rule == "lucky" && out.casts[1].mod == "m",
        "rules: casts carry ability, rule and mod");
    out = rules::Evaluate(mods, cast, state, high, 8);
    Check(out.allowed == 3 && out.blockedByChance == 1 && out.casts.size() == 2, "rules: the chance roll blocks only the 50% rule");
    state.healthPercent = 80.0;
    out = rules::Evaluate(mods, cast, state, low, 8);
    Check(out.blockedByCondition == 1 && out.casts.size() == 2 && out.casts[0].abilityId == "C",
        "rules: a false condition blocks its rule only");
    soulash::conditions::Snapshot blank;
    out = rules::Evaluate(mods, cast, blank, low, 8);
    Check(out.blockedByCondition == 2 && out.casts.size() == 1 && out.casts[0].abilityId == "D",
        "rules: unknown state fails closed unless on_unavailable=active");
    out = rules::Evaluate(mods, cast, state, low, 1);
    Check(out.casts.size() == 1 && out.notes.size() == 1, "rules: the per-event cast limit holds and is noted");
    out = rules::Evaluate(mods, attack, state, low, 8);
    Check(out.matched == 1 && out.casts.size() == 1 && out.casts[0].abilityId == "E", "rules: attack rules");
    state.weaponClass = std::string("Sword");
    out = rules::Evaluate(mods, attack, state, low, 8);
    Check(out.matched == 1 && out.casts.empty(), "rules: attack rule blocked by weapon class");

    // The other events: each rule listens to exactly its own kind.
    rules::Event damaged{rules::EventKind::Damaged, ""};
    rules::Event kill{rules::EventKind::Kill, ""};
    rules::Event turn{rules::EventKind::Turn, ""};
    rules::Event dodge{rules::EventKind::Dodge, ""};
    Check(rules::AnyRuleFor(mods, damaged) && rules::AnyRuleFor(mods, kill) && rules::AnyRuleFor(mods, turn)
            && !rules::AnyRuleFor(mods, dodge),
        "rules: pre-check knows the new event kinds");
    state.healthPercent = 30.0;
    out = rules::Evaluate(mods, damaged, state, low, 8);
    Check(out.matched == 1 && out.casts.size() == 1 && out.casts[0].abilityId == "F", "rules: damaged rule fires");
    out = rules::Evaluate(mods, kill, state, low, 8);
    Check(out.matched == 1 && out.casts.size() == 1 && out.casts[0].abilityId == "G", "rules: kill rule fires");
    out = rules::Evaluate(mods, turn, state, low, 8);
    Check(out.matched == 1 && out.blockedByCondition == 1 && out.casts.empty(), "rules: turn rule unknown stacks -> fails closed");
    state.stackers = std::map<std::string, double>{{"flag", 2.0}};
    out = rules::Evaluate(mods, turn, state, low, 8);
    Check(out.casts.size() == 1 && out.casts[0].abilityId == "H", "rules: turn rule fires on live stacks");
    Check(std::string(rules::EventName(rules::EventKind::Crit)) == "crit"
            && std::string(rules::EventName(rules::EventKind::Block)) == "block",
        "rules: event names");

    // Effects triggers: a delayed cast needs a turn source.
    rules::ModRules fx;
    fx.mod = "m";
    Check(fx.effects.Load("{\"spec\":1,\"mod\":\"m\",\"requires\":[\"delay\",\"on_kill\"],\"triggers\":["
                          "{\"id\":\"later\",\"on\":{\"kill\":true},\"do\":{\"delay\":2,\"do\":{\"cast\":{\"spell\":\"X\"}}}},"
                          "{\"id\":\"now\",\"on\":{\"kill\":true},\"do\":{\"cast\":{\"spell\":\"Y\"}}}]}", error),
        "rules: effects document loads");
    fx.hasEffects = true;
    std::vector<rules::ModRules> fxMods;
    fxMods.push_back(std::move(fx));
    out = rules::Evaluate(fxMods, kill, state, low, 8);
    Check(out.matched == 2 && out.casts.size() == 1 && out.delayed.empty() && out.notes.size() == 1
            && out.notes[0].find("dropped") != std::string::npos,
        "rules: without a turn source the delayed cast is dropped and noted");
    out = rules::Evaluate(fxMods, kill, state, low, 8, true);
    Check(out.casts.size() == 1 && out.delayed.size() == 1 && out.delayed[0].delayTurns == 2
            && out.delayed[0].abilityId.find("X") != std::string::npos && out.delayed[0].rule == "later" && out.notes.empty(),
        "rules: with a turn source the delayed cast is returned with its delay");

    // A file requiring an unbacked predicate is unavailable as a whole.
    rules::ModRules strict;
    strict.mod = "s";
    Check(strict.conditions.Load("{\"spec\":1,\"requires\":[\"biome\"],\"triggers\":[{\"id\":\"x\",\"on\":{\"cast\":\"A\"},"
                                  "\"cast\":\"Y\",\"when\":{\"health_percent\":{\"max\":50}}}]}", error),
        "rules: strict document loads");
    strict.hasConditions = true;
    state.healthPercent = 10.0;
    out = rules::Evaluate(std::vector<rules::ModRules>{std::move(strict)}, cast, state, low, 8);
    Check(out.matched == 1 && out.blockedByCondition == 1 && out.casts.empty(),
        "rules: requires on an unbacked predicate fails closed");
}
