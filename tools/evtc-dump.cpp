// Development tool: prints what the parser reads from a log. Build: see README (any C++17 compiler).
#include <cstdio>
#include "../src/evtc.h"

int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: evtc-dump <log.zevtc>"); return 1; }
    std::string narrow = argv[1];
    ParsedFight f = ParseEvtcFile(std::wstring(narrow.begin(), narrow.end()));
    if (!f.ok) { std::printf("erreur : %s\n", f.error.c_str()); return 1; }
    std::printf("%s fightId=%u boss=%s success=%d hpLeft=%.1f duration=%llums start=%llu\n", f.wvw ? "WvW" : "PvE", f.fightId, f.boss.c_str(), f.success, f.hpLeft, (unsigned long long)f.durationMs, (unsigned long long)f.startUnix);
    std::printf("lu : %d agents, %d joueurs (%d vus, %d avec équipe), %d couleur(s), %zu événements, pov=%d\n", f.agentsRead, f.playersRead, f.playersSeen, f.playersWithTeam, f.coloursKnown, f.eventsRead, f.hasPov);
    for (const auto& t : f.teams) {
        std::printf("team %s%s players=%d kills=%d deaths=%d downs=%d damage=%llu :", t.name.c_str(), t.pov ? " (pov)" : "", t.players, t.kills, t.deaths, t.downs, (unsigned long long)t.damage);
        for (const auto& sc : t.specs) std::printf(" %s×%d", SpecShort(sc.spec).c_str(), sc.count);
        std::printf("\n");
    }
    std::printf("squad %zu:\n", f.squad.size());
    for (const auto& p : f.squad) std::printf("  %-20s %-14s g%d dmg=%llu downs=%d deaths=%d kills=%d%s\n", p.name.c_str(), p.spec.c_str(), p.subgroup, (unsigned long long)p.damage, p.downs, p.deaths, p.kills, p.pov ? " (moi)" : "");
    return 0;
}
