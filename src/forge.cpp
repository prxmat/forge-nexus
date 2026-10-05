#include "forge.h"
#include <thread>
#include "arcdps.h"
#include "live.h"

#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <sstream>

#include "json.hpp"
#include "boons.h"
#include "text.h"

using json = nlohmann::json;

ForgeState g_state;

// What a support wants on screen from the start; the other boons are listed, off.
std::vector<AuraRule> DefaultAuras() {
    std::vector<AuraRule> out;
    for (const auto& boon : BOONS) {
        AuraRule rule;
        rule.buff = boon.id;
        rule.on = boon.id == 1187 || boon.id == 30328 || boon.id == 740 || boon.id == 1122;
        rule.minStacks = boon.id == 740 ? 10 : 1;
        out.push_back(rule);
    }
    return out;
}

static std::wstring Widen(const std::string& text) {
    if (text.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size);
    return out;
}

// Splits "https://host/path?query" for WinHTTP.
static bool SplitUrl(const std::string& url, std::wstring& host, std::wstring& path, bool& secure, INTERNET_PORT& port) {
    std::wstring wide = Widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t hostBuffer[256]{}, pathBuffer[2048]{};
    parts.lpszHostName = hostBuffer; parts.dwHostNameLength = 256;
    parts.lpszUrlPath = pathBuffer; parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wide.c_str(), (DWORD)wide.size(), 0, &parts)) return false;
    host.assign(parts.lpszHostName, parts.dwHostNameLength);
    path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    port = parts.nPort;
    return true;
}

static int HttpRequest(const wchar_t* verb, const std::string& url, const std::string& token, const std::string& body, std::string& out) {
    std::wstring host, path; bool secure; INTERNET_PORT port;
    if (!SplitUrl(url, host, path, secure, port)) return -1;
    HINTERNET session = WinHttpOpen(L"forge-nexus/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return -1;
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 15000);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    int status = -1;
    if (connect) {
        HINTERNET request = WinHttpOpenRequest(connect, verb, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (request) {
            // The Forge token goes to Forge only: other hosts (deltaconnected) get no Authorization header.
            std::wstring headers = (token.empty() ? L"" : L"Authorization: Bearer " + Widen(token) + L"\r\n") + std::wstring(L"Content-Type: application/json\r\nAccept: */*\r\n");
            if (WinHttpSendRequest(request, headers.c_str(), (DWORD)headers.size(), body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) && WinHttpReceiveResponse(request, nullptr)) {
                DWORD code = 0, size = sizeof(code);
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
                status = (int)code;
                out.clear();
                for (;;) {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(request, &available) || !available) break;
                    std::string chunk(available, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(request, &chunk[0], available, &read)) break;
                    out.append(chunk, 0, read);
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return status;
}

int HttpGet(const std::string& url, const std::string& token, std::string& out) { return HttpRequest(L"GET", url, token, "", out); }

std::string g_addonVersion;

void ReportError(const std::string& message, const std::string& contextJson) {
    static std::mutex mutex;
    static std::map<std::string, uint64_t> lastAt;
    {
        std::lock_guard<std::mutex> lock(mutex);
        uint64_t now = GetTickCount64();
        auto it = lastAt.find(message);
        if (it != lastAt.end() && now - it->second < 10 * 60 * 1000) return;
        lastAt[message] = now;
    }
    Settings settings;
    { std::lock_guard<std::mutex> lock(g_state.mutex); settings = g_state.settings; }
    if (settings.token.empty()) return;
    json body;
    body["tool"] = "addon";
    body["version"] = g_addonVersion;
    body["message"] = message;
    try { body["context"] = json::parse(contextJson); } catch (...) { body["context"] = json::object(); }
    std::string out;
    HttpPostJson(settings.forgeUrl + "/api/client-errors", settings.token, body.dump(), out);
}
int HttpPostJson(const std::string& url, const std::string& token, const std::string& body, std::string& out) { return HttpRequest(L"POST", url, token, body, out); }

void LoadSettings(const std::string& path) {
    std::ifstream file(path);
    if (!file) return;
    try {
        json data = json::parse(file);
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.settings.forgeUrl = data.value("forge_url", g_state.settings.forgeUrl);
        g_state.settings.token = data.value("token", "");
        g_state.settings.rosterId = data.value("roster_id", "");
        g_state.settings.logsDir = data.value("logs_dir", "");
        g_state.settings.showWindow = data.value("show_window", true);
        g_state.settings.fontScale = data.value("font_scale", 1.0f);
        g_state.settings.squadOnly = data.value("squad_only", false);
        g_state.settings.sortByDamage = data.value("sort_by_damage", false);
        g_state.settings.shortNames = data.value("short_names", false);
        g_state.settings.showWidget = data.value("show_widget", true);
        g_state.settings.showStrip = data.value("show_strip", true);
        g_state.settings.gameBuild = data.value("game_build", 0u);
        if (data.contains("wvw_teams") && data["wvw_teams"].is_object()) {
            std::lock_guard<std::mutex> tlock(g_teamColoursMutex);
            for (const auto& [id, colour] : data["wvw_teams"].items()) if (colour.is_string()) g_teamColours[(uint32_t)std::stoul(id)] = colour.get<std::string>();
        }
        g_state.settings.stripX = data.value("strip_x", 330.0f);
        g_state.settings.stripY = data.value("strip_y", 30.0f);
        g_state.settings.panelX = data.value("panel_x", 20.0f);
        g_state.settings.panelY = data.value("panel_y", 60.0f);
        g_state.settings.panelBars = data.value("panel_bars", true);
        g_state.settings.panelNames = data.value("panel_names", true);
        g_state.settings.panelIcons = data.value("panel_icons", true);
        g_state.settings.panelClassDamage = data.value("panel_class_damage", true);
        g_state.settings.panelTotal = data.value("panel_total", true);
        g_state.settings.panelKd = data.value("panel_kd", false);
        g_state.settings.panelDeaths = data.value("panel_deaths", true);
        g_state.settings.panelDowns = data.value("panel_downs", true);
        g_state.settings.panelDamage = data.value("panel_damage", true);
        g_state.settings.panelAlpha = data.value("panel_alpha", 0.88f);
        g_state.settings.panelAllies = data.value("panel_allies", true);
        g_state.settings.panelAlliesTotal = data.value("panel_allies_total", true);
        g_state.settings.panelStrike = data.value("panel_strike", false);
        g_state.settings.panelCondi = data.value("panel_condi", false);
        g_state.settings.hideInCombat = data.value("hide_in_combat", false);
        Settings& s = g_state.settings;
        s.timersOn = data.value("timers_on", true);
        s.timersFromTaimi = data.value("timers_from_taimi", true);
        s.centerText = data.value("center_text", true);
        s.speakSounds = data.value("speak_sounds", true);
        s.speakAlerts = data.value("speak_alerts", true);
        s.speakWarnings = data.value("speak_warnings", true);
        s.warnAt = data.value("warn_at", 4);
        // Before the countdown sounds (0.9.3): the voice read at 3 s; it moves to 4 s, ahead of the "wuh" at 3, 2, 1.
        if (!data.contains("countdown_sound") && s.warnAt == 3) s.warnAt = 4;
        s.countdownSound = data.value("countdown_sound", std::string("sirene"));
        s.dueSound = data.value("due_sound", std::string(data.value("beep_due", false) ? "bip" : ""));
        s.alertSound = data.value("alert_sound", std::string());
        s.countdownFrom = data.value("countdown_from", 3);
        s.countdownEvery = std::max(1, data.value("countdown_every", 1));
        s.soundVolume = data.value("sound_volume", 80);
        s.warningSounds.clear();
        if (data.contains("warning_sounds") && data["warning_sounds"].is_object()) {
            for (const auto& [key, value] : data["warning_sounds"].items()) {
                if (!value.is_object()) continue;
                WarningSound sound;
                sound.countdown = value.value("countdown", std::string("default"));
                sound.due = value.value("due", std::string("default"));
                s.warningSounds[key] = sound;
            }
        }
        s.timerVoice = data.value("timer_voice", "");
        s.auraVoice = data.value("aura_voice", "");
        s.voiceVolume = data.value("voice_volume", 90);
        s.voiceRate = data.value("voice_rate", 1);
        s.timersOff.clear();
        if (data.contains("timers_off") && data["timers_off"].is_array()) for (const auto& id : data["timers_off"]) if (id.is_string()) s.timersOff.push_back(id.get<std::string>());
        s.timersOptIn.clear();
        if (data.contains("timers_opt_in") && data["timers_opt_in"].is_array()) for (const auto& id : data["timers_opt_in"]) if (id.is_string()) s.timersOptIn.push_back(id.get<std::string>());
        s.alertsLocked = data.value("alerts_locked", true);
        s.barsX = data.value("bars_x", 40.0f);
        s.barsY = data.value("bars_y", 380.0f);
        s.barWidth = data.value("bar_width", 300.0f);
        s.aurasX = data.value("auras_x", 760.0f);
        s.aurasY = data.value("auras_y", 640.0f);
        s.auraSize = data.value("aura_size", 44.0f);
        s.centerY = data.value("center_y", 0.28f);
        s.centerScale = data.value("center_scale", 1.5f);
        s.aurasOn = data.value("auras_on", true);
        if (data.contains("auras") && data["auras"].is_array()) {
            // Saved rules first, in their order; boons added since then come after, as their defaults.
            std::vector<AuraRule> rules;
            for (const auto& item : data["auras"]) {
                if (!item.is_object()) continue;
                AuraRule rule;
                rule.buff = item.value("buff", 0u);
                if (!FindBoon(rule.buff)) continue;
                bool known = false;
                for (const auto& other : rules) if (other.buff == rule.buff) known = true;
                if (known) continue;
                rule.on = item.value("on", false);
                rule.show = item.value("show", 0);
                rule.minStacks = item.value("min_stacks", 1);
                rule.combatOnly = item.value("combat_only", true);
                rule.sound = item.value("sound", 0);
                rules.push_back(rule);
            }
            for (const auto& fallback : DefaultAuras()) {
                bool known = false;
                for (const auto& rule : rules) if (rule.buff == fallback.buff) known = true;
                if (!known) rules.push_back(fallback);
            }
            s.auras = rules;
        }
    } catch (...) {}
}

// The caller holds g_state.mutex (std::mutex cannot be taken twice by the same thread: the game would freeze).
void SaveSettingsLocked(const std::string& path) {
    json data;
    {
        data["forge_url"] = g_state.settings.forgeUrl;
        data["token"] = g_state.settings.token;
        data["roster_id"] = g_state.settings.rosterId;
        data["logs_dir"] = g_state.settings.logsDir;
        data["show_window"] = g_state.settings.showWindow;
        data["font_scale"] = g_state.settings.fontScale;
        data["squad_only"] = g_state.settings.squadOnly;
        data["sort_by_damage"] = g_state.settings.sortByDamage;
        data["short_names"] = g_state.settings.shortNames;
        data["show_widget"] = g_state.settings.showWidget;
        data["show_strip"] = g_state.settings.showStrip;
        data["game_build"] = g_state.settings.gameBuild;
        { std::lock_guard<std::mutex> tlock(g_teamColoursMutex); json teams = json::object(); for (const auto& [id, colour] : g_teamColours) teams[std::to_string(id)] = colour; data["wvw_teams"] = teams; }
        data["strip_x"] = g_state.settings.stripX;
        data["strip_y"] = g_state.settings.stripY;
        data["panel_x"] = g_state.settings.panelX;
        data["panel_y"] = g_state.settings.panelY;
        data["panel_bars"] = g_state.settings.panelBars;
        data["panel_names"] = g_state.settings.panelNames;
        data["panel_icons"] = g_state.settings.panelIcons;
        data["panel_class_damage"] = g_state.settings.panelClassDamage;
        data["panel_total"] = g_state.settings.panelTotal;
        data["panel_kd"] = g_state.settings.panelKd;
        data["panel_deaths"] = g_state.settings.panelDeaths;
        data["panel_downs"] = g_state.settings.panelDowns;
        data["panel_damage"] = g_state.settings.panelDamage;
        data["panel_alpha"] = g_state.settings.panelAlpha;
        data["panel_allies"] = g_state.settings.panelAllies;
        data["panel_allies_total"] = g_state.settings.panelAlliesTotal;
        data["panel_strike"] = g_state.settings.panelStrike;
        data["panel_condi"] = g_state.settings.panelCondi;
        data["hide_in_combat"] = g_state.settings.hideInCombat;
        const Settings& s = g_state.settings;
        data["timers_on"] = s.timersOn;
        data["timers_from_taimi"] = s.timersFromTaimi;
        data["center_text"] = s.centerText;
        data["speak_sounds"] = s.speakSounds;
        data["speak_alerts"] = s.speakAlerts;
        data["speak_warnings"] = s.speakWarnings;
        data["warn_at"] = s.warnAt;
        data["countdown_sound"] = s.countdownSound;
        data["due_sound"] = s.dueSound;
        data["alert_sound"] = s.alertSound;
        data["countdown_from"] = s.countdownFrom;
        data["countdown_every"] = s.countdownEvery;
        data["sound_volume"] = s.soundVolume;
        json sounds = json::object();
        for (const auto& [key, sound] : s.warningSounds) if (sound.countdown != "default" || sound.due != "default") sounds[key] = { { "countdown", sound.countdown }, { "due", sound.due } };
        data["warning_sounds"] = sounds;
        data["timer_voice"] = s.timerVoice;
        data["aura_voice"] = s.auraVoice;
        data["voice_volume"] = s.voiceVolume;
        data["voice_rate"] = s.voiceRate;
        data["timers_off"] = s.timersOff;
        data["timers_opt_in"] = s.timersOptIn;
        data["alerts_locked"] = s.alertsLocked;
        data["bars_x"] = s.barsX;
        data["bars_y"] = s.barsY;
        data["bar_width"] = s.barWidth;
        data["auras_x"] = s.aurasX;
        data["auras_y"] = s.aurasY;
        data["aura_size"] = s.auraSize;
        data["center_y"] = s.centerY;
        data["center_scale"] = s.centerScale;
        data["auras_on"] = s.aurasOn;
        json rules = json::array();
        for (const auto& rule : s.auras) rules.push_back({ { "buff", rule.buff }, { "on", rule.on }, { "show", rule.show }, { "min_stacks", rule.minStacks }, { "combat_only", rule.combatOnly }, { "sound", rule.sound } });
        data["auras"] = rules;
    }
    std::ofstream file(path);
    file << data.dump(2);
}

void SaveSettings(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    SaveSettingsLocked(path);
}

static std::vector<std::string> Strings(const json& value) {
    std::vector<std::string> out;
    if (value.is_array()) for (const auto& item : value) if (item.is_string()) out.push_back(item.get<std::string>());
    return out;
}

static LiveBoss ReadBoss(const json& value) {
    LiveBoss boss;
    boss.id = value.value("id", "");
    boss.label = value.value("label", "");
    boss.wing = value.value("wing", "");
    if (value.contains("myPlace") && value["myPlace"].is_string()) { boss.myPlace = value["myPlace"].get<std::string>(); boss.hasPlace = true; }
    if (value.contains("guide") && value["guide"].is_object()) {
        const json& guide = value["guide"];
        boss.hasGuide = true;
        boss.guideUrl = guide.value("url", "");
        boss.essentials = Strings(guide["essentials"]);
        boss.squad = Strings(guide["squad"]);
        for (const auto& item : guide["mechanics"]) boss.mechanics.push_back({ item.value("name", ""), item.contains("aka") && item["aka"].is_string() ? item["aka"].get<std::string>() : "", Strings(item["body"]), Strings(item["tips"]) });
        for (const auto& item : guide["sections"]) boss.sections.push_back({ item.value("title", ""), Strings(item["body"]), Strings(item["tips"]) });
    }
    if (value.contains("goals")) for (const auto& item : value["goals"]) boss.goals.push_back({ item.value("label", ""), item.value("state", ""), item.value("met", false) });
    for (const auto& item : value["slots"]) boss.slots.push_back({ item.value("label", ""), item.contains("player") && item["player"].is_string() ? item["player"].get<std::string>() : "", item.value("me", false) });
    return boss;
}

bool PollNight() {
    Settings settings;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        settings = g_state.settings;
    }
    if (settings.token.empty()) {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.status = "Aucun token Forge : colle-le dans les options de l'addon.";
        g_state.tokenOk = false;
        return false;
    }
    std::string body;
    // Forge keeps the state of arcdps per member, so Bienvenue can say when it is missing or outdated.
    std::string query = std::string("?arcdps=") + ArcdpsCode(ArcdpsVerdict()) + (settings.rosterId.empty() ? "" : "&roster=" + settings.rosterId);
    int status = HttpGet(settings.forgeUrl + "/api/live/night" + query, settings.token, body);
    // Forge's guides are typographic French (’ – … œ): the window's font only has Latin-1.
    body = ForFont(body);
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (status == 401) { g_state.status = "Token Forge inconnu ou révoqué."; g_state.tokenOk = false; g_state.hasNight = false; return false; }
    if (status != 200) {
        std::string detail;
        try { json data = json::parse(body); detail = data.value("error", ""); } catch (...) {}
        g_state.status = status < 0 ? "Forge injoignable (réseau)." : "Forge répond " + std::to_string(status) + (detail.empty() ? "." : " : " + detail);
        return true;
    }
    try {
        json data = json::parse(body);
        g_state.tokenOk = true;
        if (!data.contains("night") || data["night"].is_null()) {
            g_state.hasNight = false;
            g_state.status = data.value("reason", "Aucune soirée.");
            return true;
        }
        const json& n = data["night"];
        LiveNight night;
        night.member = n.value("member", "");
        night.canPlan = n.value("canPlan", false);
        if (n.contains("roster") && n["roster"].is_object()) { night.rosterId = n["roster"].value("id", ""); night.rosterName = n["roster"].value("name", ""); }
        night.date = n.value("date", "");
        night.day = n.value("day", "");
        night.phase = n.value("phase", "waiting");
        night.version = n.contains("version") && n["version"].is_string() ? n["version"].get<std::string>() : "";
        night.liveUrl = n.value("liveUrl", "");
        night.killed = n.value("killed", 0);
        if (n.contains("rosters")) for (const auto& item : n["rosters"]) night.rosters.push_back({ item.value("id", ""), item.value("name", ""), item.value("tonight", false), item.value("live", false) });
        if (n.contains("bosses")) for (const auto& item : n["bosses"]) night.bosses.push_back({ item.value("id", ""), item.value("label", ""), item.value("killed", false), item.value("on", false) });
        if (n.contains("current") && n["current"].is_object()) { night.current = ReadBoss(n["current"]); night.hasCurrent = true; }
        if (n.contains("next") && n["next"].is_object()) { night.next = ReadBoss(n["next"]); night.hasNext = true; }
        g_state.night = night;
        g_state.hasNight = true;
        g_state.status = "Connecté : " + night.member;
    } catch (const std::exception& error) {
        g_state.status = std::string("Réponse de Forge illisible (") + error.what() + ") : " + body.substr(0, 80);
        std::thread([message = std::string("Réponse de /api/live/night illisible : ") + error.what()]() { ReportError(message); }).detach();
    }
    return true;
}

void PollStats() {
    Settings settings;
    { std::lock_guard<std::mutex> lock(g_state.mutex); settings = g_state.settings; }
    if (settings.token.empty()) return;
    std::string body;
    if (HttpGet(settings.forgeUrl + "/api/live/stats", settings.token, body) != 200) return;
    body = ForFont(body);
    try {
        json data = json::parse(body);
        PveStats pve; WvwStats wvw;
        bool hasPve = data.contains("pve") && data["pve"].is_object();
        bool hasWvw = data.contains("wvw") && data["wvw"].is_object();
        if (hasPve) {
            const json& p = data["pve"];
            pve.boss = p.value("boss", ""); pve.success = p.value("success", false); pve.hpLeft = p.value("hpLeft", 0.0);
            pve.durationMs = p.value("durationMs", 0); pve.url = p.value("url", "");
            const json& me = p["me"];
            pve.dps = me.contains("dps") && me["dps"].is_number() ? me["dps"].get<int>() : -1;
            pve.rank = me.contains("rank") && me["rank"].is_number() ? me["rank"].get<int>() : 0;
            pve.squad = me.value("squad", 0); pve.deaths = me.value("deaths", 0); pve.downs = me.value("downs", 0);
            if (me.contains("fellFirst") && me["fellFirst"].is_string()) pve.fellFirst = me["fellFirst"].get<std::string>();
            for (const auto& m : me["mechanics"]) pve.mechanics.push_back({ m.value("name", ""), m.value("count", 0) });
            const json& night = p["night"];
            pve.nightPlayed = night.value("played", 0); pve.nightKills = night.value("kills", 0); pve.nightDeaths = night.value("deaths", 0);
        }
        if (hasWvw) {
            const json& w = data["wvw"];
            wvw.title = w.value("title", "");
            const json& e = w["evening"];
            wvw.fights = e.value("fights", 0); wvw.kills = e.value("kills", 0); wvw.deaths = e.value("deaths", 0);
            wvw.squadDowns = e.value("squadDowns", 0); wvw.enemyDowns = e.value("enemyDowns", 0); wvw.seconds = e.value("seconds", 0);
            if (w.contains("last") && w["last"].is_object()) {
                const json& l = w["last"];
                wvw.hasLast = true;
                wvw.lastMap = l.value("map", ""); wvw.lastUrl = l.value("url", "");
                wvw.lastDuration = l.value("duration", 0); wvw.lastSquad = l.value("squad", 0); wvw.lastAllies = l.value("allies", 0);
                wvw.lastKills = l.value("kills", 0); wvw.lastEnemyDowns = l.value("enemyDowns", 0); wvw.lastDeaths = l.value("deaths", 0); wvw.lastSquadDowns = l.value("squadDowns", 0);
                wvw.lastDamage = l.value("damage", 0.0);
                wvw.lastEnemyDamage = l.contains("enemyDamage") && l["enemyDamage"].is_number() ? l["enemyDamage"].get<double>() : -1;
                wvw.lastStability = l.contains("stability") && l["stability"].is_number() ? l["stability"].get<int>() : -1;
                if (l.contains("teams") && l["teams"].is_object()) for (auto it = l["teams"].begin(); it != l["teams"].end(); ++it) wvw.teams.push_back({ it.key(), it.value().is_number() ? it.value().get<int>() : 0 });
                for (const auto& f : l["findings"]) wvw.findings.push_back({ f.value("tone", ""), f.value("text", "") });
                auto readPlayer = [](const json& pl) { FightPlayer fp; fp.account = pl.value("account", ""); fp.name = pl.value("name", ""); fp.profession = pl.value("profession", ""); fp.seconds = pl.value("seconds", 0); fp.damage = pl.value("damage", 0.0); fp.dps = pl.value("dps", 0.0); fp.kills = pl.value("kills", 0); fp.enemyDowns = pl.value("enemyDowns", 0); fp.downs = pl.value("downs", 0); fp.deaths = pl.value("deaths", 0); fp.strips = pl.value("strips", 0); fp.cleanses = pl.value("cleanses", 0); fp.stability = pl.value("stability", 0.0); fp.dist = pl.contains("distToCommander") && pl["distToCommander"].is_number() ? pl["distToCommander"].get<double>() : -1; return fp; };
                if (l.contains("players")) for (const auto& pl : l["players"]) wvw.lastPlayers.push_back(readPlayer(pl));
                if (l.contains("me") && l["me"].is_object()) { wvw.lastMe = readPlayer(l["me"]); wvw.hasLastMe = true; }
            }
            if (w.contains("me") && w["me"].is_object()) {
                const json& m = w["me"];
                wvw.hasMe = true;
                wvw.meRole = m.value("role", ""); wvw.meDamage = m.value("damage", 0.0); wvw.meDps = m.value("dps", 0.0);
                wvw.meDowns = m.value("downs", 0); wvw.meDeaths = m.value("deaths", 0); wvw.meKills = m.value("kills", 0);
                wvw.meDist = m.contains("distToCommander") && m["distToCommander"].is_number() ? m["distToCommander"].get<double>() : -1;
                wvw.meStability = m.contains("stability") && m["stability"].is_number() ? m["stability"].get<double>() : -1;
            }
        }
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.pve = pve; g_state.hasPve = hasPve;
        g_state.wvw = wvw; g_state.hasWvw = hasWvw;
    } catch (...) {}
}

void StartWvwEvening() {
    Settings settings;
    { std::lock_guard<std::mutex> lock(g_state.mutex); if (g_state.busy) return; g_state.busy = true; g_state.error.clear(); settings = g_state.settings; }
    std::string out;
    int status = HttpPostJson(settings.forgeUrl + "/api/live/session", settings.token, "{}", out);
    out = ForFont(out);
    if (status != 200) {
        std::string message = "Forge a refusé (" + std::to_string(status) + ").";
        try { json data = json::parse(out); message = data.value("error", message); } catch (...) {}
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.error = message;
    }
    PollStats();
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.busy = false;
}

void SendOp(const std::string& op, const std::string& bossId) {
    Settings settings; std::string roster, date;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.busy || !g_state.hasNight) return;
        g_state.busy = true;
        g_state.error.clear();
        settings = g_state.settings;
        roster = g_state.night.rosterId;
        date = g_state.night.date;
    }
    json body = { { "roster", roster }, { "date", date }, { "op", op } };
    if (!bossId.empty()) body["bossId"] = bossId;
    std::string out;
    int status = HttpPostJson(settings.forgeUrl + "/api/live/op", settings.token, body.dump(), out);
    out = ForFont(out);
    if (status != 200) {
        std::string message = "Forge a refusé (" + std::to_string(status) + ").";
        try { json data = json::parse(out); message = data.value("error", message); } catch (...) {}
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.error = message;
    }
    PollNight();
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.busy = false;
}
