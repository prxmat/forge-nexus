#include "auras.h"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "arcevents.h"

namespace {
// arcdps' buff statechanges (2025 on); before, applies and removes rode on plain combat events.
constexpr uint8_t SC_BuffInitial = 18, SC_BuffApply = 69, SC_BuffChange = 70, SC_BuffRemoveSingle = 71, SC_BuffRemoveAll = 72;
constexpr uint8_t REMOVE_ALL = 1, REMOVE_SINGLE = 2;
constexpr size_t MAX_STACKS = 25;

struct Tracked {
    std::unordered_map<uint32_t, uint64_t> stacks; // Intensity: trackable id → when it ends.
    uint64_t queueEnd = 0;                          // Duration: when the queued stacks run out.
    uint32_t peak = 0;
    uint32_t localId = 0x80000000u;                 // Ids for stacks arcdps did not number.
};

std::mutex mutex;
std::unordered_map<uint32_t, Tracked> buffs;
bool everBuff = false;
std::vector<uint64_t> recent(256, 0);
size_t recentAt = 0;

bool Tracks(uint32_t buff) {
    switch (buff) {
    case 740: case 725: case 1187: case 30328: case 717: case 718: case 719: case 726: case 743: case 1122: case 26980: case 873: case 5974: return true;
    default: return false;
    }
}

bool Self(const arc::AgentShort* agent) { return agent && agent->self; }

void Prune(Tracked& t, uint64_t now) {
    for (auto it = t.stacks.begin(); it != t.stacks.end();) it = it->second <= now ? t.stacks.erase(it) : std::next(it);
}

void Apply(uint32_t buff, uint32_t stackId, int32_t value, uint32_t overstack, bool initial, uint64_t now) {
    if (value <= 0) return;
    Tracked& t = buffs[buff];
    if (AuraIntensity(buff)) {
        Prune(t, now);
        t.stacks[stackId ? stackId : t.localId++] = now + (uint32_t)value;
        while (t.stacks.size() > MAX_STACKS) {
            auto shortest = std::min_element(t.stacks.begin(), t.stacks.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            t.stacks.erase(shortest);
        }
    } else {
        uint32_t added = (uint32_t)value;
        if (overstack && overstack < added) added -= overstack;
        // A stack already there when the log starts: it may be one we follow already, so it does not add up.
        t.queueEnd = initial ? std::max(t.queueEnd, now + added) : std::max(t.queueEnd, now) + added;
    }
}

void Change(uint32_t buff, uint32_t stackId, int32_t difference, uint32_t newDuration, uint64_t now) {
    auto found = buffs.find(buff);
    if (found == buffs.end()) return;
    Tracked& t = found->second;
    if (AuraIntensity(buff)) {
        auto it = t.stacks.find(stackId);
        if (it != t.stacks.end()) it->second = now + newDuration;
    } else if (t.queueEnd > now) {
        int64_t end = (int64_t)t.queueEnd + difference;
        t.queueEnd = (uint64_t)std::max<int64_t>((int64_t)now, end);
    }
}

void RemoveOne(uint32_t buff, uint32_t stackId, int32_t removedMs, uint64_t now) {
    auto found = buffs.find(buff);
    if (found == buffs.end()) return;
    Tracked& t = found->second;
    if (AuraIntensity(buff)) {
        Prune(t, now);
        auto it = stackId ? t.stacks.find(stackId) : t.stacks.end();
        if (it == t.stacks.end() && !t.stacks.empty()) {
            // No id: the stack whose time left is closest to what was removed.
            it = std::min_element(t.stacks.begin(), t.stacks.end(), [&](const auto& a, const auto& b) {
                return std::llabs((long long)(a.second - now) - removedMs) < std::llabs((long long)(b.second - now) - removedMs);
            });
        }
        if (it != t.stacks.end()) t.stacks.erase(it);
    } else if (t.queueEnd > now) {
        t.queueEnd = removedMs > 0 && t.queueEnd - now > (uint64_t)removedMs ? t.queueEnd - removedMs : now;
    }
}

void RemoveAll(uint32_t buff) {
    auto found = buffs.find(buff);
    if (found == buffs.end()) return;
    found->second.stacks.clear();
    found->second.queueEnd = 0;
    found->second.peak = 0;
}
} // namespace

bool AuraIntensity(uint32_t buff) { return buff == 740 || buff == 1122; }

void AurasOnEvent(void* payload) {
    const arc::EvCombatData* p = (const arc::EvCombatData*)payload;
    if (!p || !p->ev) return;
    const arc::CombatEvent* ev = p->ev;
    if (!Tracks(ev->skillId)) return;
    // The squad's boons fly by by the hundred in a zerg: only the player's go further.
    enum class Kind { None, Apply, Initial, Change, RemoveOne, RemoveAll } kind = Kind::None;
    switch (ev->isStateChange) {
    case SC_BuffApply: if (Self(p->dst)) kind = Kind::Apply; break;
    case SC_BuffInitial: if (Self(p->dst)) kind = Kind::Initial; break;
    case SC_BuffChange: if (Self(p->dst)) kind = Kind::Change; break;
    case SC_BuffRemoveSingle: if (Self(p->src)) kind = Kind::RemoveOne; break;
    case SC_BuffRemoveAll: if (Self(p->src)) kind = Kind::RemoveAll; break;
    case 0:
        if (ev->isActivation) break;
        if (ev->isBuffRemove == REMOVE_ALL) { if (Self(p->src)) kind = Kind::RemoveAll; }
        else if (ev->isBuffRemove == REMOVE_SINGLE) { if (Self(p->src)) kind = Kind::RemoveOne; }
        else if (!ev->isBuffRemove && ev->buff && ev->buffDmg == 0 && ev->value > 0 && Self(p->dst)) kind = Kind::Apply;
        break;
    default: break;
    }
    if (kind == Kind::None) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (p->id) {
        for (uint64_t seen : recent) if (seen == p->id) return;
        recent[recentAt++ % recent.size()] = p->id;
    }
    everBuff = true;
    uint64_t now = GetTickCount64();
    const uint32_t buff = ev->skillId;
    const bool modern = ev->isStateChange != 0;
    switch (kind) {
    case Kind::Apply: Apply(buff, modern ? ev->pad : 0, ev->value, modern ? 0 : ev->overstackValue, false, now); break;
    case Kind::Initial: Apply(buff, ev->pad, ev->value, 0, true, now); break;
    case Kind::Change: Change(buff, ev->pad, ev->value, ev->overstackValue, now); break;
    case Kind::RemoveOne: RemoveOne(buff, modern ? ev->pad : 0, ev->value, now); break;
    case Kind::RemoveAll: RemoveAll(buff); break;
    case Kind::None: break;
    }
}

std::map<uint32_t, BoonState> AurasSnapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<uint32_t, BoonState> out;
    uint64_t now = GetTickCount64();
    for (auto& [buff, t] : buffs) {
        BoonState state;
        if (AuraIntensity(buff)) {
            Prune(t, now);
            state.stacks = (int)t.stacks.size();
            for (const auto& stack : t.stacks) state.remainingMs = std::max(state.remainingMs, (uint32_t)(stack.second - now));
        } else {
            state.remainingMs = t.queueEnd > now ? (uint32_t)(t.queueEnd - now) : 0;
            state.stacks = state.remainingMs ? 1 : 0;
        }
        t.peak = state.remainingMs ? std::max(t.peak, state.remainingMs) : 0;
        state.peakMs = t.peak;
        out[buff] = state;
    }
    return out;
}

bool AurasEverBuff() {
    std::lock_guard<std::mutex> lock(mutex);
    return everBuff;
}

void AurasClear() {
    std::lock_guard<std::mutex> lock(mutex);
    buffs.clear();
}
