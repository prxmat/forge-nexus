#include "live.h"

#include <windows.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

std::mutex g_teamColoursMutex;
std::map<uint32_t, std::string> g_teamColours;

namespace {
// arcdps' realtime structures (its extension API): the event is the log's 64-byte one, the agent is short.
#pragma pack(push, 1)
struct CombatEvent {
    uint64_t time, srcAgent, dstAgent;
    int32_t value, buffDmg;
    uint32_t overstackValue, skillId;
    uint16_t srcInstid, dstInstid, srcMasterInstid, dstMasterInstid;
    uint8_t iff, buff, result, isActivation, isBuffRemove, isNinety, isFifty, isMoving, isStateChange, isFlanking, isShields, isOffCycle;
    uint32_t pad;
};
#pragma pack(pop)
static_assert(sizeof(CombatEvent) == 64, "arcdps combat event is 64 bytes");
struct AgentShort { char* name; uintptr_t id; uint32_t prof, elite, self; uint16_t team; };
struct EvCombatData { CombatEvent* ev; AgentShort* src; AgentShort* dst; char* skillname; uint64_t id, revision; };

enum State : uint8_t { SC_None = 0, SC_EnterCombat = 1, SC_ExitCombat = 2, SC_ChangeUp = 3, SC_ChangeDead = 4, SC_ChangeDown = 5, SC_HealthUpdate = 8, SC_LogStart = 9, SC_LogEnd = 10, SC_Reward = 17 };
enum Result : uint8_t { R_Normal = 0, R_Critical = 1, R_Glance = 2, R_KillingBlow = 8 };
constexpr uint32_t NON_PLAYER = 0xFFFFFFFF;
constexpr uint64_t END_AFTER_MS = 8000;  // Out of combat this long: the fight is over.
constexpr uint64_t LINGER_MS = 6000;     // After the log's end, how long the live view waits for the file.

struct LiveAgent {
    std::string name, account, spec;
    uint16_t team = 0;
    int subgroup = 0;
    bool self = false, player = false, seen = false;
    uint64_t damage = 0, taken = 0; // taken: on non-players, to find the boss.
    int downs = 0, deaths = 0, kills = 0;
    double hp = -1;
};

struct LiveState {
    std::mutex mutex;
    // Ids of the last events, to drop a repeat (two bridges relaying the same arcdps event).
    std::vector<uint64_t> recentIds = std::vector<uint64_t>(512, 0);
    size_t recentAt = 0;
    bool active = false, finished = false, everEvent = false, wvwMap = false, success = false;
    uint64_t startTick = 0, finishedTick = 0, outOfCombatTick = 0;
    std::unordered_map<uintptr_t, LiveAgent> agents; // The squad roster outlives fights.
    std::unordered_map<uint16_t, uintptr_t> byInstid;
} g_live;

void Reset(LiveState& s) {
    s.active = false; s.finished = false; s.success = false;
    s.startTick = 0; s.finishedTick = 0;
    s.byInstid.clear();
    for (auto& [id, a] : s.agents) { a.damage = 0; a.taken = 0; a.downs = 0; a.deaths = 0; a.kills = 0; a.seen = false; a.hp = -1; }
}

void Begin(LiveState& s) {
    Reset(s);
    s.active = true;
    s.startTick = GetTickCount64();
}

// What a combat event says about an agent: name, kind, team.
LiveAgent* Note(LiveState& s, const AgentShort* ag, uint16_t instid) {
    if (!ag || !ag->id) return nullptr;
    LiveAgent& a = s.agents[ag->id];
    if (ag->name && ag->name[0] && a.name.empty()) a.name = ag->name;
    if (ag->elite == NON_PLAYER) a.player = false;
    else if (ag->prof >= 1 && ag->prof <= 9) { a.player = true; if (a.spec.empty()) a.spec = SpecFromIds(ag->prof, ag->elite); }
    if (ag->self) a.self = true;
    if (ag->team) a.team = ag->team;
    if (instid) s.byInstid[instid] = ag->id;
    a.seen = true;
    return &a;
}

LiveAgent* Master(LiveState& s, uint16_t masterInstid) {
    if (!masterInstid) return nullptr;
    auto it = s.byInstid.find(masterInstid);
    if (it == s.byInstid.end()) return nullptr;
    auto agent = s.agents.find(it->second);
    return agent == s.agents.end() ? nullptr : &agent->second;
}
} // namespace

void LiveOnEvent(void* payload) {
    const EvCombatData* p = (const EvCombatData*)payload;
    if (!p) return;
    std::lock_guard<std::mutex> lock(g_live.mutex);
    LiveState& s = g_live;
    s.everEvent = true;
    if (p->id) {
        for (uint64_t seen : s.recentIds) if (seen == p->id) return;
        s.recentIds[s.recentAt++ % s.recentIds.size()] = p->id;
    }
    const CombatEvent* ev = p->ev;
    if (!ev) {
        // Agent tracking: src is the character, dst carries account, profession, elite, self and subgroup.
        const AgentShort* src = p->src;
        const AgentShort* dst = p->dst;
        if (!src || src->elite == 1) return; // Target change.
        if (src->prof) {
            LiveAgent& a = s.agents[src->id];
            if (src->name && src->name[0]) a.name = src->name;
            if (dst) {
                if (dst->name && dst->name[0]) { a.account = dst->name; if (a.account[0] == ':') a.account.erase(0, 1); }
                if (dst->prof >= 1 && dst->prof <= 9) { a.player = true; a.spec = SpecFromIds(dst->prof, dst->elite); }
                a.self = dst->self != 0;
                a.subgroup = (int)dst->team;
            }
            if (src->team) a.team = src->team;
        } else {
            auto it = s.agents.find(src->id);
            if (it != s.agents.end() && !it->second.self) it->second.subgroup = 0;
        }
        return;
    }
    switch (ev->isStateChange) {
    case SC_LogStart: Begin(s); return;
    case SC_LogEnd: if (s.active) { s.finished = true; s.finishedTick = GetTickCount64(); } return;
    case SC_Reward: s.success = true; return;
    case SC_HealthUpdate: { if (LiveAgent* a = Note(s, p->src, ev->srcInstid); a && !a->player) a->hp = ev->dstAgent / 100.0; return; }
    case SC_ChangeDead: case SC_ChangeDown: {
        if (!s.active || s.finished) return;
        LiveAgent* a = Note(s, p->src, ev->srcInstid);
        if (!a || !a->player) return;
        if (ev->isStateChange == SC_ChangeDead) a->deaths++; else a->downs++;
        return;
    }
    case SC_None: break;
    default: return;
    }
    if (ev->isActivation || ev->isBuffRemove) return;
    if (ev->result != R_Normal && ev->result != R_Critical && ev->result != R_Glance && ev->result != R_KillingBlow) return;
    int32_t damage = ev->buff == 0 ? ev->value : ev->buff == 1 ? ev->buffDmg : 0;
    if (damage <= 0 && ev->result != R_KillingBlow) return;
    // No log start seen (arcdps may not send it): the first hit opens the fight.
    if (!s.active || s.finished) { if (s.finished) return; Begin(s); }
    LiveAgent* attacker = Note(s, p->src, ev->srcInstid);
    LiveAgent* target = Note(s, p->dst, ev->dstInstid);
    if (attacker && !attacker->player) attacker = Master(s, ev->srcMasterInstid); // A minion's damage is its owner's.
    if (!attacker || !attacker->player || !target) return;
    if (s.wvwMap) {
        if (!target->player || !attacker->team || target->team == attacker->team) return;
        if (damage > 0) attacker->damage += damage;
        if (ev->result == R_KillingBlow) attacker->kills++;
    } else {
        if (target->player) return;
        if (damage > 0) { attacker->damage += damage; target->taken += damage; }
    }
}

void LiveTick(bool inCombat, bool wvwMap) {
    std::lock_guard<std::mutex> lock(g_live.mutex);
    LiveState& s = g_live;
    s.wvwMap = wvwMap;
    uint64_t now = GetTickCount64();
    if (inCombat) s.outOfCombatTick = 0;
    else if (!s.outOfCombatTick) s.outOfCombatTick = now;
    if (!s.active) return;
    if (s.finished) { if (now - s.finishedTick > LINGER_MS) s.active = false; return; }
    if (s.outOfCombatTick && now - s.outOfCombatTick > END_AFTER_MS) { s.finished = true; s.finishedTick = now; }
}

void LiveDismiss() {
    std::lock_guard<std::mutex> lock(g_live.mutex);
    g_live.active = false;
}

bool LiveEverEvent() {
    std::lock_guard<std::mutex> lock(g_live.mutex);
    return g_live.everEvent;
}

bool LiveSnapshot(ParsedFight& out) {
    std::lock_guard<std::mutex> lock(g_live.mutex);
    const LiveState& s = g_live;
    if (!s.active) return false;
    std::map<uint32_t, std::string> colours;
    { std::lock_guard<std::mutex> clock(g_teamColoursMutex); colours = g_teamColours; }
    auto colourOf = [&](uint16_t team) -> std::string { if (!team) return ""; auto it = colours.find(team); return it != colours.end() ? it->second : "Équipe " + std::to_string(team); };
    ParsedFight f;
    f.ok = true;
    f.file = s.finished ? "Direct · terminé" : "● En direct";
    f.wvw = s.wvwMap;
    f.fightId = s.wvwMap ? 1 : 0;
    f.success = s.success;
    f.durationMs = (s.finished ? s.finishedTick : GetTickCount64()) - s.startTick;
    uint16_t povTeam = 0;
    for (const auto& [id, a] : s.agents) if (a.self) povTeam = a.team;
    auto lineOf = [](const LiveAgent& a) { PlayerLine line; line.name = a.name; line.account = a.account; line.spec = a.spec; line.subgroup = a.subgroup; line.damage = a.damage; line.downs = a.downs; line.deaths = a.deaths; line.kills = a.kills; line.pov = a.self; return line; };
    std::map<std::string, TeamSummary> teams;
    std::map<std::string, std::map<std::string, SpecCount>> teamSpecs;
    const LiveAgent* boss = nullptr;
    for (const auto& [id, a] : s.agents) {
        if (!a.player) { if (a.seen && a.taken && (!boss || a.taken > boss->taken)) boss = &a; continue; }
        if (!a.seen && !(a.subgroup > 0 && !f.wvw)) continue;
        std::string colour = colourOf(a.team);
        if (f.wvw && !colour.empty()) {
            auto& team = teams[colour];
            team.players++; team.deaths += a.deaths; team.downs += a.downs; team.kills += a.kills; team.damage += a.damage;
            auto& spec = teamSpecs[colour][a.spec];
            spec.count++; spec.damage += a.damage; spec.deaths += a.deaths; spec.downs += a.downs;
        }
        bool squad = a.subgroup > 0 && (!f.wvw || a.team == povTeam || !a.team);
        if (squad || a.self) f.squad.push_back(lineOf(a));
        if (a.self) { f.me = lineOf(a); f.hasMe = true; }
    }
    if (!f.wvw) {
        if (boss) { f.boss = boss->name.empty() ? "Boss" : boss->name; f.hpLeft = s.success ? 0 : boss->hp; }
        else f.boss = "Combat";
    }
    for (auto& [name, team] : teams) {
        team.name = name;
        team.pov = name == colourOf(povTeam);
        for (auto& [spec, entry] : teamSpecs[name]) { entry.spec = spec; if (entry.count) team.specs.push_back(entry); }
        std::sort(team.specs.begin(), team.specs.end(), [](const SpecCount& a, const SpecCount& b) { return a.count > b.count; });
        f.teams.push_back(team);
    }
    std::sort(f.teams.begin(), f.teams.end(), [](const TeamSummary& a, const TeamSummary& b) { return a.pov != b.pov ? a.pov : a.players > b.players; });
    std::sort(f.squad.begin(), f.squad.end(), [](const PlayerLine& a, const PlayerLine& b) { return a.damage > b.damage; });
    out = f;
    return true;
}
