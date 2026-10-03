// Native check of the boon tracking (src/auras.cpp) with made-up arcdps events, old and 2025 formats. From the root:
//   clang++ -std=c++17 -Itools/stub -Isrc tools/auras-check.cpp src/auras.cpp -o build/native/auras-check && build/native/auras-check
#include <cstdio>
#include <string>

#include "arcevents.h"
#include "auras.h"

uint64_t g_fakeTicks = 1000000;
static int failures = 0;
static uint64_t nextId = 1;
static void Expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) failures++;
}

static arc::AgentShort me{ (char*)"Maori Adalrik", 1001, 1, 64, 1, 0 };
static arc::AgentShort ally{ (char*)"Ally", 1002, 2, 0, 0, 0 };

static void Send(uint8_t statechange, uint32_t buff, int32_t value, arc::AgentShort* src, arc::AgentShort* dst, uint32_t stack = 0, uint8_t remove = 0, uint8_t isBuff = 0, int32_t buffDmg = 0, uint32_t overstack = 0) {
    arc::CombatEvent ev{};
    ev.isStateChange = statechange;
    ev.skillId = buff;
    ev.value = value;
    ev.pad = stack;
    ev.isBuffRemove = remove;
    ev.buff = isBuff;
    ev.buffDmg = buffDmg;
    ev.overstackValue = overstack;
    arc::EvCombatData data{ &ev, src, dst, nullptr, nextId++, 1 };
    AurasOnEvent(&data);
}

int main() {
    // 2025 statechanges: 69 apply, 70 change, 71 remove one, 72 remove all.
    Send(69, 1187, 5000, &ally, &me, 11);
    Send(69, 1187, 3000, &ally, &me, 12);
    auto s = AurasSnapshot();
    Expect(s[1187].remainingMs == 8000, "quickness queues: 5 s + 3 s = 8 s");
    g_fakeTicks += 2000;
    s = AurasSnapshot();
    Expect(s[1187].remainingMs == 6000 && s[1187].peakMs == 8000, "two seconds later: 6 s left, sweep peak 8 s");
    Send(71, 1187, 3000, &me, &ally, 12, 2);
    Expect(AurasSnapshot()[1187].remainingMs == 3000, "one stack stripped: 3 s left");
    Send(72, 1187, 3000, &me, &ally, 0, 1);
    Expect(AurasSnapshot()[1187].remainingMs == 0, "all removed: gone");
    // Applied to someone else: ignored.
    Send(69, 30328, 5000, &me, &ally, 21);
    Expect(AurasSnapshot()[30328].remainingMs == 0, "alacrity on an ally is not mine");
    // Might: intensity, one stack per id, expiring on its own.
    for (uint32_t i = 0; i < 12; i++) Send(69, 740, i < 6 ? 4000 : 10000, &ally, &me, 100 + i);
    s = AurasSnapshot();
    Expect(s[740].stacks == 12, "12 stacks of might");
    g_fakeTicks += 5000;
    s = AurasSnapshot();
    Expect(s[740].stacks == 6 && s[740].remainingMs == 5000, "the 4 s stacks ran out: 6 left, 5 s on the longest");
    Send(71, 740, 5000, &me, &ally, 106, 2);
    Expect(AurasSnapshot()[740].stacks == 5, "one stack removed by id");
    // 70 carries the new duration in overstack_value.
    {
        arc::CombatEvent ev{};
        ev.isStateChange = 70; ev.skillId = 740; ev.value = -4000; ev.overstackValue = 1000; ev.pad = 108;
        arc::EvCombatData data{ &ev, &ally, &me, nullptr, nextId++, 1 };
        AurasOnEvent(&data);
    }
    g_fakeTicks += 1500;
    Expect(AurasSnapshot()[740].stacks == 4, "a stack shortened to 1 s ran out");
    // Cap: 25 stacks.
    for (uint32_t i = 0; i < 40; i++) Send(69, 740, 20000, &ally, &me, 500 + i);
    Expect(AurasSnapshot()[740].stacks == 25, "might caps at 25 stacks");
    // Before 2025: applies on combat events (buff, buff_dmg 0, value = duration), removes with is_buffremove.
    Send(0, 1122, 3000, &ally, &me, 0, 0, 1, 0, 0);
    Send(0, 1122, 3000, &ally, &me, 0, 0, 1, 0, 0);
    Expect(AurasSnapshot()[1122].stacks == 2, "old format: 2 stacks of stability");
    Send(0, 1122, 3000, &me, &ally, 0, 2, 1);
    Expect(AurasSnapshot()[1122].stacks == 1, "old format: one stack removed");
    Send(0, 725, 10000, &ally, &me, 0, 0, 1, 0, 4000);
    Expect(AurasSnapshot()[725].remainingMs == 6000, "old format: overstack is not added");
    // A repeat of the same event (two bridges) counts once.
    {
        arc::CombatEvent ev{};
        ev.isStateChange = 69; ev.skillId = 717; ev.value = 4000; ev.pad = 900;
        arc::EvCombatData data{ &ev, &ally, &me, nullptr, 424242, 1 };
        AurasOnEvent(&data);
        AurasOnEvent(&data);
    }
    Expect(AurasSnapshot()[717].remainingMs == 4000, "the same event twice counts once");
    // Initial buffs at a log's start do not add to what is already followed.
    Send(18, 717, 4000, &ally, &me, 900);
    Expect(AurasSnapshot()[717].remainingMs == 4000, "buff initial does not double a known stack");
    Expect(AurasEverBuff(), "buff events seen");
    AurasClear();
    Expect(AurasSnapshot().empty(), "map change clears");
    std::printf(failures ? "%d FAILED\n" : "all good\n", failures);
    return failures ? 1 : 0;
}
