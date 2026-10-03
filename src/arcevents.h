// arcdps' realtime structures (its extension API), as Nexus' ArcDPS Integration relays them: the event is the log's
// 64-byte one, the agent is the short realtime one. Shared by the live fight and the auras.
#pragma once
#include <cstdint>

namespace arc {
#pragma pack(push, 1)
struct CombatEvent {
    uint64_t time, srcAgent, dstAgent;
    int32_t value, buffDmg;
    uint32_t overstackValue, skillId;
    uint16_t srcInstid, dstInstid, srcMasterInstid, dstMasterInstid;
    uint8_t iff, buff, result, isActivation, isBuffRemove, isNinety, isFifty, isMoving, isStateChange, isFlanking, isShields, isOffCycle;
    uint32_t pad; // pad61..64: the buff stack's trackable id on buff statechanges.
};
#pragma pack(pop)
static_assert(sizeof(CombatEvent) == 64, "arcdps combat event is 64 bytes");
struct AgentShort { char* name; uintptr_t id; uint32_t prof, elite, self; uint16_t team; };
struct EvCombatData { CombatEvent* ev; AgentShort* src; AgentShort* dst; char* skillname; uint64_t id, revision; };
} // namespace arc
