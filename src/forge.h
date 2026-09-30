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
    bool hasGuide = false;
    bool hasPlace = false;
};

struct LiveNight {
    std::string member, rosterId, rosterName, date, day, phase, version, liveUrl;
    bool canPlan = false;
    int killed = 0;
    struct Boss { std::string id, label; bool killed = false, on = false; };
    std::vector<Boss> bosses;
    LiveBoss current, next;
    bool hasCurrent = false, hasNext = false;
};

struct Settings {
    std::string forgeUrl = "https://forge-lbm.vercel.app";
    std::string token;
    bool showWindow = true;
    float fontScale = 1.0f;
};

// Shared between the render thread and the poller.
struct ForgeState {
    std::mutex mutex;
    Settings settings;
    LiveNight night;
    bool hasNight = false;
    std::string status;   // "Connecté : X", or what blocks.
    std::string error;    // Last error of a click.
    bool busy = false;    // A click is being sent.
    bool tokenOk = false;
};

extern ForgeState g_state;

void LoadSettings(const std::string& path);
void SaveSettings(const std::string& path);

// One poll: fetches /api/live/night and updates the state; returns false when the token is refused.
bool PollNight();
// A lead's click: op is start | go | kill | undo | skip | end.
void SendOp(const std::string& op, const std::string& bossId);

// HTTP helpers (WinHTTP). Return the status code, body in `out`.
int HttpGet(const std::string& url, const std::string& token, std::string& out);
int HttpPostJson(const std::string& url, const std::string& token, const std::string& body, std::string& out);
