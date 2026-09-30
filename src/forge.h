// Forge addon for Nexus: talks to Forge (Le Bus Magique) and keeps the live night in memory for the window.
#pragma once
#include <mutex>
#include <string>
#include <vector>

struct LiveBoss {
    std::string id, label, wing, myPlace, guideUrl;
    std::vector<std::string> essentials, squad;
    struct Mechanic { std::string name, aka; std::vector<std::string> body, tips; };
    std::vector<Mechanic> mechanics;
    struct Section { std::string title; std::vector<std::string> body, tips; };
    std::vector<Section> sections;
    struct Slot { std::string label, player; bool me = false; };
    std::vector<Slot> slots;
    struct Goal { std::string label, state; bool met = false; };
    std::vector<Goal> goals;
    bool hasGuide = false;
    bool hasPlace = false;
};

struct LiveNight {
    std::string member, rosterId, rosterName, date, day, phase, version, liveUrl;
    bool canPlan = false;
    int killed = 0;
    struct Boss { std::string id, label; bool killed = false, on = false; };
    std::vector<Boss> bosses;
    struct Roster { std::string id, name; bool tonight = false, live = false; };
    std::vector<Roster> rosters;
    LiveBoss current, next;
    bool hasCurrent = false, hasNext = false;
};

struct PveStats {
    std::string boss, url, fellFirst;
    bool success = false;
    double hpLeft = 0;
    int durationMs = 0, dps = -1, rank = 0, squad = 0, deaths = 0, downs = 0;
    std::vector<std::pair<std::string, int>> mechanics;
    int nightPlayed = 0, nightKills = 0, nightDeaths = 0;
};

struct FightPlayer { std::string account, name, profession; int seconds = 0; double damage = 0, dps = 0; int kills = 0, enemyDowns = 0, downs = 0, deaths = 0, strips = 0, cleanses = 0; double stability = 0, dist = -1; };

struct WvwStats {
    std::string title, lastMap, lastUrl;
    int fights = 0, kills = 0, deaths = 0, squadDowns = 0, enemyDowns = 0, seconds = 0;
    bool hasLast = false;
    int lastDuration = 0, lastSquad = 0, lastAllies = 0, lastKills = 0, lastEnemyDowns = 0, lastDeaths = 0, lastSquadDowns = 0;
    double lastDamage = 0, lastEnemyDamage = -1;
    int lastStability = -1;
    std::vector<std::pair<std::string, int>> teams;
    std::vector<std::pair<std::string, std::string>> findings; // tone, text
    std::vector<FightPlayer> lastPlayers;
    bool hasLastMe = false;
    FightPlayer lastMe;
    bool hasMe = false;
    std::string meRole;
    double meDamage = 0, meDps = 0, meDist = -1, meStability = -1;
    int meDowns = 0, meDeaths = 0, meKills = 0;
};

struct Settings {
    std::string forgeUrl = "https://forge-lbm.vercel.app";
    std::string token;
    // The roster chosen in the window when the member plays in several; empty = Forge's pick.
    std::string rosterId;
    // arcdps log folder; empty = the default under Documents.
    std::string logsDir;
    bool showWindow = true;
    float fontScale = 1.0f;
    // Combats tab and widget.
    bool squadOnly = false, sortByDamage = false, shortNames = false, showWidget = true, showStrip = true, hideInCombat = false;
    // Panel contents, as Fight Analysis lets one pick them.
    bool panelBars = true, panelNames = true, panelIcons = true, panelClassDamage = true, panelTotal = true, panelKd = false, panelDeaths = true, panelDowns = true, panelDamage = true;
    float panelAlpha = 0.88f;
    // Where the strip and the panel sit on screen, once moved.
    float stripX = 330.0f, stripY = 30.0f, panelX = 20.0f, panelY = 60.0f;
    // The game's build seen last session: a change means a game update, which may have broken arcdps.
    uint32_t gameBuild = 0;
};

// Shared between the render thread and the poller.
struct ForgeState {
    std::mutex mutex;
    Settings settings;
    LiveNight night;
    bool hasNight = false;
    PveStats pve;
    WvwStats wvw;
    bool hasPve = false, hasWvw = false;
    std::string status;   // "Connecté : X", or what blocks.
    std::string error;    // Last error of a click.
    bool busy = false;    // A click is being sent.
    bool tokenOk = false;
};

extern ForgeState g_state;

void LoadSettings(const std::string& path);
void SaveSettings(const std::string& path);
// Same, for a caller that already holds g_state.mutex.
void SaveSettingsLocked(const std::string& path);

// One poll: fetches /api/live/night and updates the state; returns false when the token is refused.
bool PollNight();
// One poll of /api/live/stats: the latest PvE pull and the McM evening.
void PollStats();
// "Nouvelle soirée McM": the fights sent from now on go to a fresh evening.
void StartWvwEvening();
// A lead's click: op is start | go | kill | undo | skip | end.
void SendOp(const std::string& op, const std::string& bossId);

// HTTP helpers (WinHTTP). Return the status code, body in `out`.
int HttpGet(const std::string& url, const std::string& token, std::string& out);
int HttpPostJson(const std::string& url, const std::string& token, const std::string& body, std::string& out);
