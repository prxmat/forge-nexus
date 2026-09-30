#include "evtc.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include "thirdparty/miniz/miniz.h"

namespace {

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

enum State : uint8_t { SC_None = 0, SC_EnterCombat = 1, SC_ExitCombat = 2, SC_ChangeUp = 3, SC_ChangeDead = 4, SC_ChangeDown = 5, SC_HealthUpdate = 8, SC_LogStart = 9, SC_LogEnd = 10, SC_PointOfView = 13, SC_Reward = 17, SC_TeamChange = 22, SC_IdToGuid = 46, SC_WvwTeams = 74 };
enum Result : uint8_t { R_Normal = 0, R_Critical = 1, R_Glance = 2, R_KillingBlow = 8 };

struct Agent {
    uint64_t address = 0;
    uint32_t prof = 0;
    int32_t elite = 0;
    uint16_t instid = 0;
    bool player = false;
    std::string name, account, spec, team;
    int subgroup = 0;
    bool seen = false;
};

const std::unordered_map<int, const char*> PROFESSIONS = { {1, "Guardian"}, {2, "Warrior"}, {3, "Engineer"}, {4, "Ranger"}, {5, "Thief"}, {6, "Elementalist"}, {7, "Mesmer"}, {8, "Necromancer"}, {9, "Revenant"} };
const std::unordered_map<int, const char*> ELITES = {
    {5, "Druid"}, {7, "Daredevil"}, {18, "Berserker"}, {27, "Dragonhunter"}, {34, "Reaper"}, {40, "Chronomancer"}, {43, "Scrapper"}, {48, "Tempest"}, {52, "Herald"}, {55, "Soulbeast"}, {56, "Weaver"}, {57, "Holosmith"}, {58, "Deadeye"}, {59, "Mirage"}, {60, "Scourge"}, {61, "Spellbreaker"}, {62, "Firebrand"}, {63, "Renegade"}, {64, "Harbinger"}, {65, "Willbender"}, {66, "Virtuoso"}, {67, "Catalyst"}, {68, "Bladesworn"}, {69, "Vindicator"}, {70, "Mechanist"}, {71, "Specter"}, {72, "Untamed"}, {73, "Troubadour"}, {74, "Paragon"}, {75, "Amalgam"}, {76, "Ritualist"}, {77, "Antiquary"}, {78, "Galeshot"}, {79, "Conduit"}, {80, "Evoker"}, {81, "Luminary"},
};
const std::unordered_map<std::string, const char*> SHORTS = {
    {"Guardian", "Gdn"}, {"Dragonhunter", "Dgh"}, {"Firebrand", "Fbd"}, {"Willbender", "Wbd"}, {"Luminary", "Lum"}, {"Warrior", "War"}, {"Berserker", "Brs"}, {"Spellbreaker", "Spb"}, {"Bladesworn", "Bds"}, {"Paragon", "Par"}, {"Engineer", "Eng"}, {"Scrapper", "Scr"}, {"Holosmith", "Hls"}, {"Mechanist", "Mec"}, {"Amalgam", "Amg"}, {"Ranger", "Rgr"}, {"Druid", "Dru"}, {"Soulbeast", "Slb"}, {"Untamed", "Unt"}, {"Galeshot", "Gsh"}, {"Thief", "Thf"}, {"Daredevil", "Dar"}, {"Deadeye", "Ded"}, {"Specter", "Spe"}, {"Antiquary", "Ant"}, {"Elementalist", "Ele"}, {"Tempest", "Tmp"}, {"Weaver", "Wea"}, {"Catalyst", "Cat"}, {"Evoker", "Evo"}, {"Mesmer", "Mes"}, {"Chronomancer", "Chr"}, {"Mirage", "Mir"}, {"Virtuoso", "Vir"}, {"Troubadour", "Trb"}, {"Necromancer", "Nec"}, {"Reaper", "Rea"}, {"Scourge", "Scg"}, {"Harbinger", "Har"}, {"Ritualist", "Rit"}, {"Revenant", "Rev"}, {"Herald", "Her"}, {"Renegade", "Ren"}, {"Vindicator", "Vin"}, {"Conduit", "Con"},
};
// Raid and strike bosses by species id (the evtc header's fight id).
const std::unordered_map<uint16_t, const char*> BOSSES = {
    {15438, "Gardien de la Vallée"}, {15429, "Gorseval"}, {15375, "Sabetha"}, {16123, "Slothasor"}, {16088, "Trio de bandits"}, {16115, "Matthias"}, {16253, "Escorte"}, {16235, "Keep Construct"}, {16247, "Twisted Castle"}, {16246, "Xera"},
    {17194, "Cairn"}, {17172, "Mursaat Overseer"}, {17188, "Samarog"}, {17154, "Deimos"}, {19767, "Soulless Horror"}, {19828, "Rivière des âmes"}, {19691, "Statue de la Mort"}, {19536, "Statue de la Peine"}, {19651, "Statue de la Peine"}, {19844, "Statue de la Peine"}, {19450, "Dhuum"},
    {43974, "Conjured Amalgamate"}, {21105, "Twin Largos"}, {21089, "Twin Largos"}, {20934, "Qadim"}, {21970, "La porte"}, {22006, "Cardinal Adina"}, {21964, "Cardinal Sabir"}, {22000, "Qadim l'Inégalé"},
    {26725, "Greer"}, {26774, "Decima"}, {26712, "Ura"},
    {22154, "Chevalier de glace"}, {22343, "Chevalier de glace"}, {22481, "Chevalier de glace"}, {22492, "Fraenir"}, {22521, "Boneskinner"}, {22711, "Voix et Griffe"}, {22836, "Voix et Griffe"}, {22315, "Voix et Griffe"}, {22321, "Whisper of Jormag"},
    {24033, "Aetherblade Hideout"}, {23957, "Xunlai Jade Junkyard"}, {24485, "Kaineng Overlook"}, {24266, "Harvest Temple"}, {25413, "Old Lion's Court"}, {25414, "Old Lion's Court"}, {25415, "Old Lion's Court"}, {25705, "Cosmic Observatory"}, {25989, "Temple of Febe"},
    {16199, "Golem"}, {19645, "Golem"}, {19676, "Golem"}, {19844, "Golem"}, {19845, "Golem"}, {19839, "Golem"}, {19851, "Golem"}, {19947, "Golem"}, {19962, "Golem"},
};

std::vector<char> ReadWholeFile(const std::wstring& path) {
    std::vector<char> bytes;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return bytes;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 200LL * 1024 * 1024) {
        bytes.resize((size_t)size.QuadPart);
        DWORD read = 0;
        size_t done = 0;
        while (done < bytes.size() && ::ReadFile(file, bytes.data() + done, (DWORD)(bytes.size() - done), &read, nullptr) && read) done += read;
        bytes.resize(done);
    }
    CloseHandle(file);
    return bytes;
}

// A .zevtc is a zip holding one evtc; a .evtc is the evtc itself.
std::vector<char> Unpack(const std::vector<char>& raw) {
    if (raw.size() >= 4 && std::memcmp(raw.data(), "EVTC", 4) == 0) return raw;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, raw.data(), raw.size(), 0)) return {};
    std::vector<char> out;
    for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&zip); index++) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_is_directory || stat.m_uncomp_size > 200ULL * 1024 * 1024) continue;
        out.resize((size_t)stat.m_uncomp_size);
        if (!mz_zip_reader_extract_to_mem(&zip, index, out.data(), out.size(), 0)) out.clear();
        break;
    }
    mz_zip_reader_end(&zip);
    return out;
}

std::string GuidHex(uint64_t first, uint64_t last) {
    static const char hex[] = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < 8; i++) { uint8_t b = (first >> (i * 8)) & 0xFF; s += hex[b >> 4]; s += hex[b & 0xF]; }
    for (int i = 0; i < 8; i++) { uint8_t b = (last >> (i * 8)) & 0xFF; s += hex[b >> 4]; s += hex[b & 0xF]; }
    return s;
}
const std::unordered_map<std::string, const char*> TEAM_GUIDS = { {"BC8AEAEF73DC8C43B041CEDFEA4D5020", "Green"}, {"5D22513B9498EB48944E94EC7A8DD657", "Red"}, {"CF6F7C254FCB184CBCCE4738EADD8388", "Blue"} };

} // namespace

const char* BossName(uint16_t speciesId) {
    auto it = BOSSES.find(speciesId);
    return it == BOSSES.end() ? nullptr : it->second;
}

std::string SpecShort(const std::string& spec) {
    auto it = SHORTS.find(spec);
    return it == SHORTS.end() ? spec.substr(0, 3) : it->second;
}

ParsedFight ParseEvtcFile(const std::wstring& path) {
    ParsedFight fight;
    std::vector<char> bytes = Unpack(ReadWholeFile(path));
    if (bytes.size() < 16) { fight.error = "fichier vide ou illisible"; return fight; }
    size_t offset = 0;
    char header[13]{};
    std::memcpy(header, bytes.data(), 12);
    offset += 12;
    uint8_t revision = (uint8_t)bytes[offset++];
    (void)revision;
    uint16_t fightId = 0;
    std::memcpy(&fightId, bytes.data() + offset, 2);
    offset += 3;
    if (std::strncmp(header, "EVTC", 4) != 0) { fight.error = "pas un log arcdps"; return fight; }
    fight.fightId = fightId;
    fight.wvw = fightId == 1;
    if (const char* name = BossName(fightId)) fight.boss = name;
    else if (!fight.wvw) fight.boss = "Boss " + std::to_string(fightId);

    // Agents: 96 bytes each. Players carry a profession 1..9 and an elite id; NPCs and gadgets do not.
    uint32_t agentCount = 0;
    if (offset + 4 > bytes.size()) { fight.error = "log tronqué"; return fight; }
    std::memcpy(&agentCount, bytes.data() + offset, 4);
    offset += 4;
    std::unordered_map<uint64_t, Agent> agents;
    for (uint32_t i = 0; i < agentCount && offset + 96 <= bytes.size(); i++, offset += 96) {
        Agent agent;
        std::memcpy(&agent.address, bytes.data() + offset, 8);
        std::memcpy(&agent.prof, bytes.data() + offset + 8, 4);
        std::memcpy(&agent.elite, bytes.data() + offset + 12, 4);
        char nameData[68];
        std::memcpy(nameData, bytes.data() + offset + 28, 68);
        nameData[67] = '\0';
        std::vector<std::string> parts;
        for (size_t start = 0; start < 68;) {
            size_t end = start;
            while (end < 68 && nameData[end] != '\0') end++;
            if (end > start) parts.emplace_back(nameData + start, nameData + end);
            start = end + 1;
            if (end >= 67) break;
        }
        auto prof = PROFESSIONS.find((int)agent.prof);
        if (prof != PROFESSIONS.end()) {
            agent.player = true;
            agent.name = parts.size() > 0 ? parts[0] : "";
            agent.account = parts.size() > 1 ? parts[1] : "";
            if (!agent.account.empty() && agent.account[0] == ':') agent.account.erase(0, 1);
            agent.subgroup = parts.size() > 2 ? atoi(parts[2].c_str()) : 0;
            auto elite = ELITES.find(agent.elite);
            agent.spec = elite != ELITES.end() ? elite->second : prof->second;
        }
        agents[agent.address] = agent;
    }
    uint32_t skillCount = 0;
    if (offset + 4 > bytes.size()) { fight.error = "log tronqué"; return fight; }
    std::memcpy(&skillCount, bytes.data() + offset, 4);
    offset += 4 + (size_t)skillCount * 68;
    if (offset > bytes.size()) { fight.error = "log tronqué"; return fight; }
    size_t eventCount = (bytes.size() - offset) / sizeof(CombatEvent);
    std::vector<CombatEvent> events(eventCount);
    std::memcpy(events.data(), bytes.data() + offset, eventCount * sizeof(CombatEvent));

    // Pass 1: identities, times, teams.
    std::unordered_map<uint16_t, Agent*> byInstid;
    std::unordered_map<uint32_t, std::string> teamColours;
    uint64_t start = UINT64_MAX, end = 0, logStart = UINT64_MAX, logEnd = 0, povAddress = 0;
    for (const auto& e : events) {
        if (e.isStateChange == SC_WvwTeams) {
            uint32_t fields[6];
            std::memcpy(&fields[0], &e.srcAgent, 8);
            std::memcpy(&fields[2], &e.dstAgent, 8);
            std::memcpy(&fields[4], &e.value, 4);
            std::memcpy(&fields[5], &e.buffDmg, 4);
            if (fields[3]) teamColours[fields[3]] = "Red";
            if (fields[4]) teamColours[fields[4]] = "Blue";
            if (fields[5]) teamColours[fields[5]] = "Green";
        } else if (e.isStateChange == SC_IdToGuid && e.skillId) {
            auto it = TEAM_GUIDS.find(GuidHex(e.srcAgent, e.dstAgent));
            if (it != TEAM_GUIDS.end()) teamColours.emplace(e.skillId, it->second);
        }
    }
    for (const auto& e : events) {
        switch (e.isStateChange) {
        case SC_LogStart: logStart = e.time; if (e.value && e.buffDmg) fight.startUnix = (uint32_t)e.value; break;
        case SC_LogEnd: logEnd = e.time; break;
        case SC_EnterCombat: start = std::min(start, e.time); break;
        case SC_ExitCombat: end = std::max(end, e.time); break;
        case SC_PointOfView: povAddress = e.srcAgent; break;
        case SC_Reward: fight.success = true; break;
        case SC_None: {
            auto src = agents.find(e.srcAgent);
            if (src != agents.end()) { src->second.instid = e.srcInstid; src->second.seen = true; byInstid[e.srcInstid] = &src->second; }
            auto dst = agents.find(e.dstAgent);
            if (dst != agents.end()) { dst->second.instid = e.dstInstid; byInstid[e.dstInstid] = &dst->second; }
            break;
        }
        case SC_TeamChange: {
            auto it = agents.find(e.srcAgent);
            if (it != agents.end() && e.value) {
                auto colour = teamColours.find((uint32_t)e.value);
                it->second.team = colour != teamColours.end() ? colour->second : std::to_string((uint32_t)e.value);
            }
            break;
        }
        default: break;
        }
    }
    if (start == UINT64_MAX) start = logStart == UINT64_MAX ? (events.empty() ? 0 : events.front().time) : logStart;
    if (end == 0) end = logEnd ? logEnd : (events.empty() ? start : events.back().time);
    fight.durationMs = end > start ? end - start : 0;

    // The boss' last health, from the agent whose species is the fight id.
    if (!fight.wvw) {
        for (const auto& e : events) {
            if (e.isStateChange != SC_HealthUpdate) continue;
            auto it = agents.find(e.srcAgent);
            if (it == agents.end() || it->second.player || (it->second.prof & 0xFFFF) != fightId) continue;
            fight.hpLeft = e.dstAgent / 100.0;
        }
        if (fight.success) fight.hpLeft = 0;
    }

    // Pass 2: falls, kills and damage, per player.
    std::unordered_map<uint64_t, PlayerLine> lines;
    std::string povTeam;
    if (auto pov = agents.find(povAddress); pov != agents.end()) povTeam = pov->second.team;
    auto lineOf = [&](Agent& agent) -> PlayerLine& {
        auto it = lines.find(agent.address);
        if (it == lines.end()) {
            PlayerLine line;
            line.name = agent.name; line.account = agent.account; line.spec = agent.spec; line.subgroup = agent.subgroup; line.pov = agent.address == povAddress;
            it = lines.emplace(agent.address, line).first;
        }
        return it->second;
    };
    std::map<std::string, TeamSummary> teams;
    std::map<std::string, std::map<std::string, int>> teamSpecs;
    for (const auto& e : events) {
        if (e.isStateChange == SC_ChangeDead || e.isStateChange == SC_ChangeDown) {
            auto it = byInstid.find(e.srcInstid);
            if (it == byInstid.end() || !it->second->player) continue;
            Agent& agent = *it->second;
            if (e.isStateChange == SC_ChangeDead) lineOf(agent).deaths++; else lineOf(agent).downs++;
            if (fight.wvw && !agent.team.empty()) { auto& team = teams[agent.team]; if (e.isStateChange == SC_ChangeDead) team.deaths++; else team.downs++; }
            continue;
        }
        if (e.isStateChange != SC_None || e.isActivation != 0 || e.isBuffRemove != 0) continue;
        if (e.result != R_Normal && e.result != R_Critical && e.result != R_Glance && e.result != R_KillingBlow) continue;
        int32_t damage = e.buff == 0 ? e.value : e.buff == 1 ? e.buffDmg : 0;
        auto src = byInstid.find(e.srcInstid);
        if (src == byInstid.end() || !src->second->player) continue;
        Agent& attacker = *src->second;
        auto dst = byInstid.find(e.dstInstid);
        Agent* target = dst == byInstid.end() ? nullptr : dst->second;
        bool vsPlayer = target && target->player;
        if (fight.wvw) {
            if (!vsPlayer || attacker.team.empty() || target->team == attacker.team) continue;
            if (damage > 0) { lineOf(attacker).damage += damage; teams[attacker.team].damage += damage; }
            if (e.result == R_KillingBlow) { lineOf(attacker).kills++; teams[attacker.team].kills++; }
        } else {
            if (vsPlayer) continue; // Damage on players (mind control, allies) is not boss damage.
            if (damage > 0) lineOf(attacker).damage += damage;
        }
    }

    // Who fought: WvW counts each team's players by specialization; both modes list the recorder's squad.
    for (auto& [address, agent] : agents) {
        if (!agent.player) continue;
        bool squad = agent.subgroup > 0 && (agent.team.empty() || agent.team == povTeam || !fight.wvw);
        if (fight.wvw && !agent.team.empty() && agent.seen) {
            auto& team = teams[agent.team];
            team.players++;
            teamSpecs[agent.team][agent.spec]++;
        }
        if (squad && agent.seen) fight.squad.push_back(lineOf(agent));
        if (address == povAddress) { fight.me = lineOf(agent); fight.hasMe = true; }
    }
    for (auto& [name, team] : teams) {
        team.name = name;
        team.pov = name == povTeam;
        for (const auto& [spec, count] : teamSpecs[name]) team.specs.push_back({ spec, count });
        std::sort(team.specs.begin(), team.specs.end(), [](const SpecCount& a, const SpecCount& b) { return a.count > b.count; });
        fight.teams.push_back(team);
    }
    std::sort(fight.teams.begin(), fight.teams.end(), [](const TeamSummary& a, const TeamSummary& b) { return a.pov != b.pov ? a.pov : a.players > b.players; });
    std::sort(fight.squad.begin(), fight.squad.end(), [](const PlayerLine& a, const PlayerLine& b) { return a.damage > b.damage; });
    fight.ok = true;
    return fight;
}
