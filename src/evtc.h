// Reads an arcdps log (evtc / zevtc) right after the game wrote it: who fought, who fell, who hit what. The
// WvW part (team colours, counts by specialization, damage between teams) follows WvW Fight Analysis by
// jake-greygoose (MIT); the PvE part adds the boss, the outcome and each squad member's damage.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct SpecCount { std::string spec; int count = 0; uint64_t damage = 0, strike = 0, condi = 0; int deaths = 0, downs = 0; };

struct TeamSummary {
    std::string name; // Red, Blue, Green (or the id when unknown).
    bool pov = false; // The recorder's team.
    int players = 0, deaths = 0, downs = 0, kills = 0;
    uint64_t damage = 0, strike = 0, condi = 0; // On enemy players; damage = strike + condi.
    std::vector<SpecCount> specs; // Sorted by count, descending.
};

struct PlayerLine {
    std::string name, account, spec;
    int subgroup = 0;
    uint64_t damage = 0, strike = 0, condi = 0; // PvE: on non-players (the boss and its adds); WvW: on enemy players.
    int downs = 0, deaths = 0, kills = 0;
    bool pov = false;
};

struct ParsedFight {
    bool ok = false;
    std::string error;
    std::string file;
    bool wvw = false;
    uint16_t fightId = 0; // Boss species id in PvE, 1 in WvW.
    std::string boss;     // PvE: the boss' name when known.
    bool success = false; // PvE: a reward event marks the kill.
    double hpLeft = -1;   // PvE: the boss' last health update, in %.
    uint64_t durationMs = 0;
    uint64_t startUnix = 0;
    std::vector<TeamSummary> teams; // WvW, POV team first.
    std::vector<PlayerLine> squad;  // The recorder's squad (subgroup > 0), sorted by damage.
    PlayerLine me;
    bool hasMe = false;
    // What the parser saw, for the Combats tab when a log yields nothing.
    int agentsRead = 0, playersRead = 0, playersWithTeam = 0, playersSeen = 0, coloursKnown = 0;
    size_t eventsRead = 0;
    bool hasPov = false;
};

ParsedFight ParseEvtcFile(const std::wstring& path);
const char* BossName(uint16_t speciesId);
// "Firebrand" → "Fbd", for tight lists.
std::string SpecShort(const std::string& spec);
// "Firebrand" → "Guardian".
std::string SpecProfession(const std::string& spec);
// arcdps' profession (1..9) and elite ids → "Firebrand"; empty for a non-player.
std::string SpecFromIds(uint32_t prof, uint32_t elite);
