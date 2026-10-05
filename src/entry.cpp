// Forge, addon Nexus : la soirée de raid en direct dans le jeu, pour les joueurs et les leads.
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <cstdio>
#include <algorithm>

#include "Nexus.h"
#include "Mumble.h"
#include "imgui/imgui.h"
#include "forge.h"
#include "icon.h"
#include "fights.h"
#include "arcdps.h"
#include "live.h"
#include "spec_icons.h"
#include "alerts.h"
#include <map>
#include <vector>

static AddonAPI_t* API = nullptr;
static AddonDefinition_t Def{};
static NexusLinkData_t* NexusLink = nullptr;
static Mumble::Data* MumbleLink = nullptr;
static std::string SettingsPath;
static std::atomic<bool> Running{ false };
static std::thread Poller;
static std::atomic<bool> PollNow{ false };

static const char* KB_TOGGLE = "KB_FORGE_TOGGLE";
static const char* KB_WIDGET = "KB_FORGE_WIDGET";
static const char* TEX_ICON = "TEX_FORGE_ICON";
static const char* TEX_ICON_HOVER = "TEX_FORGE_ICON_HOVER";
static const char* QA_ICON = "QA_FORGE";
static const char* WINDOW = "Forge";
static void SpecIcon(const std::string& spec, float size = 18.0f);
static Texture_t* SpecTexture(const std::string& spec);
static ImVec4 ProfessionColour(const std::string& spec);
static ImVec4 TeamColour(const std::string& name);
static const char* TeamLabel(const std::string& name);
// Nexus flips this on Escape (GUI_RegisterCloseOnEscape keeps the pointer): the window's own visibility flag.
static bool WindowVisible = true;

static void ToggleWindow(const char*, bool release) {
    if (release) return;
    WindowVisible = !WindowVisible;
}
static void ToggleWidget(const char*, bool release) {
    if (release) return;
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.settings.showWidget = !g_state.settings.showWidget;
    SaveSettingsLocked(SettingsPath);
}
static bool InCombat() { return MumbleLink && MumbleLink->Context.IsInCombat; }
static std::string VersionText() { return std::to_string(Def.Version.Major) + "." + std::to_string(Def.Version.Minor) + "." + std::to_string(Def.Version.Build); }
static void OnArcEventLocal(void*) { ArcdpsNoteEvent(); }
static void OnArcEventSquad(void* payload) { ArcdpsNoteEvent(); LiveOnEvent(payload); AlertsOnArcEvent(payload); }
static bool OnWvwMap() { return MumbleLink && MumbleLink->Context.MapType >= Mumble::EMapType::WvW_EternalBattlegrounds && MumbleLink->Context.MapType <= Mumble::EMapType::WvW_ObsidianSanctum; }
// The fight to show: the live one while it is on (newest slot), else the log picked in the history. g_fights.mutex held.
static const ParsedFight* ShownFight(bool newestOnly) {
    if (g_fights.liveActive && (newestOnly || g_fights.selected == 0)) return &g_fights.live;
    if (g_fights.fights.empty()) return nullptr;
    if (newestOnly) return &g_fights.fights.front();
    if (g_fights.selected >= (int)g_fights.fights.size()) g_fights.selected = 0;
    return &g_fights.fights[g_fights.selected];
}
// Once per frame: the live fight's snapshot, a few times a second.
static void LiveRefresh() {
    LiveTick(InCombat(), OnWvwMap());
    static uint64_t last = 0;
    uint64_t now = GetTickCount64();
    if (now - last < 300) return;
    last = now;
    std::lock_guard<std::mutex> flock(g_fights.mutex);
    g_fights.liveActive = LiveSnapshot(g_fights.live);
}

static std::atomic<bool> ArcCheckNow{ false };
static bool BuildNoted = false;
static void NoteGameBuild() {
    if (BuildNoted || !MumbleLink || !MumbleLink->Context.BuildID) return;
    BuildNoted = true;
    uint32_t build = MumbleLink->Context.BuildID;
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.settings.gameBuild && g_state.settings.gameBuild != build) { std::lock_guard<std::mutex> alock(g_arcdps.mutex); g_arcdps.gameUpdated = true; }
    g_state.settings.gameBuild = build;
    SaveSettingsLocked(SettingsPath);
}
static void PollLoop() {
    int tick = 0;
    while (Running) {
        NoteGameBuild();
        if (tick % 18000 == 0 || ArcCheckNow.exchange(false)) ArcdpsCheck();
        if (tick % 50 == 0 || PollNow.exchange(false)) { PollNight(); PollStats(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        tick++;
    }
}

// ---- Rendering helpers -------------------------------------------------------------------------

static const ImVec4 RED{ 0.957f, 0.263f, 0.212f, 1.0f };
static const ImVec4 GREEN{ 0.298f, 0.788f, 0.549f, 1.0f };
static const ImVec4 MUTED{ 0.651f, 0.678f, 0.733f, 1.0f };
static const ImVec4 SKY{ 0.616f, 0.753f, 0.878f, 1.0f };
static const ImVec4 GOLD{ 1.0f, 0.812f, 0.302f, 1.0f };

static void Wrapped(const std::string& text) { ImGui::TextWrapped("%s", text.c_str()); }
static void Bullet(const std::string& text) {
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextWrapped("%s", text.c_str());
}

static void RenderMechanics(const LiveBoss& boss, bool full) {
    for (const auto& mechanic : boss.mechanics) {
        ImGui::TextColored(GOLD, "%s", mechanic.name.c_str());
        if (!mechanic.aka.empty()) { ImGui::SameLine(); ImGui::TextColored(MUTED, "· %s", mechanic.aka.c_str()); }
        if (full) for (const auto& line : mechanic.body) Wrapped(line);
        for (size_t index = 0; index < mechanic.tips.size(); index++) {
            if (!full && index) break;
            Bullet(mechanic.tips[index]);
        }
        ImGui::Spacing();
    }
}

static void RenderSlots(const LiveBoss& boss) {
    if (boss.slots.empty()) { ImGui::TextColored(MUTED, "%s", "Line-up pas encore publié."); return; }
    ImGui::Columns(2, nullptr, false);
    for (const auto& slot : boss.slots) {
        ImGui::TextColored(MUTED, "%s", slot.label.c_str());
        ImGui::NextColumn();
        if (slot.me) ImGui::TextColored(SKY, "%s", slot.player.c_str());
        else if (slot.player.empty()) ImGui::TextColored(RED, "%s", "À pourvoir");
        else ImGui::TextUnformatted(slot.player.c_str());
        ImGui::NextColumn();
    }
    ImGui::Columns(1);
}

// Called with g_state.mutex held (the window holds it while it draws): SendOp takes it again in its own thread.
static void LeadButton(const char* label, const char* op, const std::string& bossId = "") {
    if (g_state.busy) { ImGui::TextColored(MUTED, "%s", label); return; }
    if (ImGui::Button(label)) { std::string o = op, b = bossId; std::thread([o, b]() { SendOp(o, b); }).detach(); }
}

// Deaths and downs in words (the font has no ✝ nor ↓): "2 morts, 1 à terre".
static std::string Fallen(int deaths, int downs) {
    std::string text;
    if (deaths) text += std::to_string(deaths) + (deaths > 1 ? " morts" : " mort");
    if (downs) text += (text.empty() ? "" : ", ") + std::to_string(downs) + " à terre";
    return text;
}
static std::string Clock(int ms) { int s = ms / 1000; char buf[16]; snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60); return buf; }
static std::string Thousands(double value) { char buf[32]; if (value >= 1000000) snprintf(buf, sizeof(buf), "%.2f M", value / 1000000); else if (value >= 1000) snprintf(buf, sizeof(buf), "%.0f k", value / 1000); else snprintf(buf, sizeof(buf), "%.0f", value); return buf; }

// A row of figure tiles: a big number, its label, a small line under it.
struct Kpi { std::string value, label, sub; ImVec4 colour; };
static void KpiRow(const std::vector<Kpi>& tiles) {
    if (tiles.empty()) return;
    const float gap = 8.0f, pad = 12.0f, between = 4.0f;
    float width = (ImGui::GetContentRegionAvail().x - gap * (tiles.size() - 1)) / tiles.size();
    float small = ImGui::GetTextLineHeight();
    if (NexusLink && NexusLink->FontBig) ImGui::PushFont((ImFont*)NexusLink->FontBig);
    float big = ImGui::GetTextLineHeight();
    if (NexusLink && NexusLink->FontBig) ImGui::PopFont();
    bool anySub = false;
    for (const auto& tile : tiles) if (!tile.sub.empty()) anySub = true;
    float height = pad + small + between + big + (anySub ? between + small : 0.0f) + pad;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (size_t index = 0; index < tiles.size(); index++) {
        const Kpi& tile = tiles[index];
        ImVec2 pos(origin.x + index * (width + gap), origin.y);
        draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(29, 35, 42, 255), 6.0f);
        draw->AddRectFilled(pos, ImVec2(pos.x + 3.0f, pos.y + height), ImGui::GetColorU32(tile.colour), 6.0f);
        float y = pos.y + pad;
        ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, y));
        ImGui::TextColored(MUTED, "%s", tile.label.c_str());
        y += small + between;
        ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, y));
        if (NexusLink && NexusLink->FontBig) ImGui::PushFont((ImFont*)NexusLink->FontBig);
        ImGui::TextColored(tile.colour, "%s", tile.value.c_str());
        if (NexusLink && NexusLink->FontBig) ImGui::PopFont();
        y += big + between;
        if (!tile.sub.empty()) { ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, y)); ImGui::TextColored(MUTED, "%s", tile.sub.c_str()); }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height + gap));
}

static void SectionTitle(const char* eyebrow, const std::string& title) {
    ImGui::Spacing();
    ImGui::TextColored(MUTED, "%s", eyebrow);
    if (NexusLink && NexusLink->FontBig) ImGui::PushFont((ImFont*)NexusLink->FontBig);
    ImGui::TextUnformatted(title.c_str());
    if (NexusLink && NexusLink->FontBig) ImGui::PopFont();
}

static void Callout(const ImVec4& colour, const std::string& text) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(pos.x + width - 12.0f);
    ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, width - 24.0f);
    ImGui::PopTextWrapPos();
    float height = size.y + 16.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec4 wash = colour; wash.w = 0.16f;
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ImGui::GetColorU32(wash), 6.0f);
    draw->AddRectFilled(pos, ImVec2(pos.x + 3.0f, pos.y + height), ImGui::GetColorU32(colour), 6.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 12.0f, pos.y + 8.0f));
    ImGui::PushTextWrapPos(pos.x + width - 12.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + 6.0f));
}

static void BarRow(const std::string& label, float fraction, const ImVec4& colour, const std::string& value) {
    float width = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine(width * 0.45f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float barWidth = width * 0.35f;
    float height = ImGui::GetTextLineHeight() * 0.6f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(pos.x, pos.y + 3.0f), ImVec2(pos.x + barWidth, pos.y + 3.0f + height), IM_COL32(40, 46, 54, 255), 3.0f);
    draw->AddRectFilled(ImVec2(pos.x, pos.y + 3.0f), ImVec2(pos.x + barWidth * std::max(0.0f, std::min(1.0f, fraction)), pos.y + 3.0f + height), ImGui::GetColorU32(colour), 3.0f);
    ImGui::Dummy(ImVec2(barWidth, height));
    ImGui::SameLine();
    ImGui::TextColored(MUTED, "%s", value.c_str());
}

// Called with g_state.mutex held: what Forge computed, a few seconds after each log landed.
// The state of arcdps, as a callout when something is wrong, one muted line otherwise.
static void ArcdpsLine(bool always) {
    ArcVerdict verdict = ArcdpsVerdict();
    bool bad = verdict == ArcVerdict::Absent || verdict == ArcVerdict::Outdated || verdict == ArcVerdict::GameUpdated || verdict == ArcVerdict::Silent;
    if (!bad && !always) return;
    if (bad) Callout(verdict == ArcVerdict::Silent || verdict == ArcVerdict::Outdated ? GOLD : RED, ArcdpsText(verdict));
    else ImGui::TextColored(MUTED, "%s", ArcdpsText(verdict).c_str());
    if (verdict == ArcVerdict::Absent || verdict == ArcVerdict::Outdated || verdict == ArcVerdict::GameUpdated) {
        if (ImGui::SmallButton("Télécharger arcdps")) ArcdpsOpenDownload();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Ouvre deltaconnected.com : d3d11.dll va dans le dossier du jeu, puis relance-le.");
        ImGui::SameLine();
    }
    if (bad || always) { if (ImGui::SmallButton("Revérifier")) ArcCheckNow = true; }
    if (always && verdict != ArcVerdict::Absent && verdict != ArcVerdict::Unknown) ImGui::TextColored(MUTED, "%s", LiveEverEvent() ? "Combat en direct : événements arcdps reçus." : "Combat en direct : aucun événement arcdps reçu pour l'instant (Nexus relaie via addons\\Nexus\\arcdps_integration64.dll, déployé tout seul).");
}
static const char* ArcdpsShort() {
    switch (ArcdpsVerdict()) {
    case ArcVerdict::Absent: return "arcdps absent";
    case ArcVerdict::Outdated: case ArcVerdict::GameUpdated: return "arcdps : mise à jour disponible";
    case ArcVerdict::Silent: return "arcdps muet en combat ?";
    default: return nullptr;
    }
}

static void RenderStats() {
    ArcdpsLine(false);
    if (!g_state.hasPve && !g_state.hasWvw) {
        Callout(MUTED, "Rien encore ce soir : les chiffres de Forge arrivent 20 à 40 s après chaque log envoyé par Forge Uploader. L'onglet Combats, lui, lit les logs tout de suite.");
        if (g_state.busy) ImGui::TextColored(MUTED, "%s", "Nouvelle sortie McM...");
        else if (ImGui::SmallButton("Nouvelle sortie McM")) std::thread([]() { StartWvwEvening(); }).detach();
        return;
    }
    if (g_state.hasPve) {
        const PveStats& p = g_state.pve;
        SectionTitle("Dernier essai · Forge", p.boss);
        ImGui::SameLine();
        if (p.success) ImGui::TextColored(GREEN, "  kill en %s", Clock(p.durationMs).c_str());
        else ImGui::TextColored(RED, "  wipe à %.1f %% · %s", p.hpLeft, Clock(p.durationMs).c_str());
        ImGui::Spacing();
        std::vector<Kpi> tiles;
        tiles.push_back({ p.dps >= 0 ? Thousands(p.dps) : "-", "DPS cible", p.rank ? std::to_string(p.rank) + "e sur " + std::to_string(p.squad) : "", SKY });
        tiles.push_back({ std::to_string(p.deaths), "Morts", std::to_string(p.downs) + " à terre", p.deaths ? RED : GREEN });
        tiles.push_back({ std::to_string(p.nightKills) + "/" + std::to_string(p.nightPlayed), "Kills ce soir", std::to_string(p.nightDeaths) + " mort(s) au total", GOLD });
        KpiRow(tiles);
        if (!p.fellFirst.empty()) Callout(RED, "Tombé(e) en premier : " + p.fellFirst);
        if (!p.mechanics.empty()) {
            ImGui::TextColored(GOLD, "%s", "Mécaniques prises");
            int top = std::max(1, p.mechanics.front().second);
            for (const auto& m : p.mechanics) BarRow(m.first, (float)m.second / (float)top, GOLD, "×" + std::to_string(m.second));
        }
        ImGui::Spacing();
    }
    if (g_state.hasWvw) {
        const WvwStats& w = g_state.wvw;
        if (g_state.hasPve) { ImGui::Separator(); ImGui::Spacing(); }
        SectionTitle("McM · Forge", w.title);
        if (g_state.busy) ImGui::TextColored(MUTED, "%s", "Nouvelle sortie McM...");
        else if (ImGui::SmallButton("Nouvelle sortie McM")) std::thread([]() { StartWvwEvening(); }).detach();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Les combats envoyés à partir de maintenant vont dans une nouvelle sortie (Forge en ouvre aussi une après 2 h sans combat).");
        if (!g_state.error.empty()) ImGui::TextColored(RED, "%s", g_state.error.c_str());
        std::vector<Kpi> tiles;
        tiles.push_back({ std::to_string(w.fights), "Combats", Clock(w.seconds * 1000) + " de combat", SKY });
        tiles.push_back({ std::to_string(w.kills), "Kills", std::to_string(w.enemyDowns) + " ennemis à terre", GREEN });
        tiles.push_back({ std::to_string(w.deaths), "Morts", std::to_string(w.squadDowns) + " à terre", RED });
        char kd[16]; snprintf(kd, sizeof(kd), "%.1f", w.deaths ? (double)w.kills / w.deaths : (double)w.kills);
        tiles.push_back({ kd, "K/D", "", w.kills >= w.deaths ? GREEN : RED });
        KpiRow(tiles);
        if (w.hasLast) {
            ImGui::TextColored(GOLD, "%s", "Dernier combat");
            ImGui::SameLine();
            ImGui::TextColored(MUTED, "· %s · %s · %d en escouade%s", w.lastMap.c_str(), Clock(w.lastDuration * 1000).c_str(), w.lastSquad, w.lastAllies ? (" + " + std::to_string(w.lastAllies) + " alliés").c_str() : "");
            if (!w.teams.empty()) {
                int shown = 0;
                for (const auto& t : w.teams) { if (shown++) ImGui::SameLine(0, 12); ImGui::TextColored(TeamColour(t.first), "%s %d", TeamLabel(t.first), t.second); }
            }
            int total = std::max(1, w.lastKills + w.lastDeaths);
            BarRow("Kills " + std::to_string(w.lastKills), (float)w.lastKills / (float)total, GREEN, std::to_string(w.lastEnemyDowns) + " à terre");
            BarRow("Morts " + std::to_string(w.lastDeaths), (float)w.lastDeaths / (float)total, RED, std::to_string(w.lastSquadDowns) + " à terre");
            double damageTotal = std::max(1.0, w.lastDamage + std::max(0.0, w.lastEnemyDamage));
            BarRow("Dégâts infligés", (float)(w.lastDamage / damageTotal), SKY, Thousands(w.lastDamage));
            if (w.lastEnemyDamage >= 0) BarRow("Dégâts reçus", (float)(w.lastEnemyDamage / damageTotal), RED, Thousands(w.lastEnemyDamage));
            if (w.lastStability >= 0) BarRow("Stabilité", w.lastStability / 100.0f, w.lastStability >= 70 ? GREEN : w.lastStability >= 40 ? GOLD : RED, std::to_string(w.lastStability) + " % du combat");
            ImGui::Spacing();
            for (const auto& f : w.findings) Callout(f.first == "bad" ? RED : f.first == "warn" ? GOLD : GREEN, f.second);
        }
        if (!w.lastPlayers.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(GOLD, "%s", "Dernier combat · escouade");
            double top = std::max(1.0, w.lastPlayers.front().damage);
            float width = ImGui::GetContentRegionAvail().x;
            int shown = 0;
            for (const auto& pl : w.lastPlayers) {
                if (shown++ >= 15) { ImGui::TextColored(MUTED, "... et %d autres", (int)w.lastPlayers.size() - 15); break; }
                bool me = w.hasLastMe && pl.account == w.lastMe.account;
                ImVec2 pos = ImGui::GetCursorScreenPos();
                float height = ImGui::GetTextLineHeight() + 6.0f;
                ImVec4 colour = me ? SKY : ProfessionColour(pl.profession);
                ImVec4 wash = colour; wash.w = 0.28f;
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(29, 35, 42, 255), 3.0f);
                draw->AddRectFilled(pos, ImVec2(pos.x + width * (float)(pl.damage / top), pos.y + height), ImGui::GetColorU32(wash), 3.0f);
                ImGui::SetCursorScreenPos(ImVec2(pos.x + 6.0f, pos.y + 3.0f));
                SpecIcon(pl.profession, ImGui::GetTextLineHeight());
                ImGui::TextColored(me ? SKY : ImVec4(0.93f, 0.94f, 0.95f, 1.0f), "%s", pl.name.c_str());
                ImGui::SameLine(0, 8); ImGui::TextColored(MUTED, "%s (%s /s)", Thousands(pl.damage).c_str(), Thousands(pl.dps).c_str());
                if (pl.kills) { ImGui::SameLine(0, 8); ImGui::TextColored(GREEN, "%d kills", pl.kills); }
                if (pl.deaths || pl.downs) { ImGui::SameLine(0, 8); ImGui::TextColored(RED, "%s", Fallen(pl.deaths, pl.downs).c_str()); }
                if (pl.stability > 0) { ImGui::SameLine(0, 8); ImGui::TextColored(GOLD, "stab %.1f", pl.stability); }
                if (pl.dist >= 0) { ImGui::SameLine(0, 8); ImGui::TextColored(pl.dist <= 600 ? MUTED : RED, "%.0f du tag", pl.dist); }
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + 3.0f));
            }
        }
        if (w.hasMe) {
            ImGui::TextColored(SKY, "Ma soirée · %s", w.meRole.c_str());
            std::vector<Kpi> mine;
            mine.push_back({ Thousands(w.meDamage), "Dégâts", Thousands(w.meDps) + " /s", SKY });
            mine.push_back({ std::to_string(w.meKills), "Kills", "", GREEN });
            mine.push_back({ std::to_string(w.meDeaths), "Morts", std::to_string(w.meDowns) + " à terre", w.meDeaths ? RED : GREEN });
            if (w.meDist >= 0) { char dist[16]; snprintf(dist, sizeof(dist), "%.0f", w.meDist); mine.push_back({ dist, "Dist. au tag", w.meDist <= 300 ? "collé" : w.meDist <= 600 ? "correct" : "loin", w.meDist <= 600 ? GREEN : GOLD }); }
            if (w.meStability >= 0) { char stab[16]; snprintf(stab, sizeof(stab), "%.1f", w.meStability); mine.push_back({ stab, "Stab générée", "stacks moyens", GOLD }); }
            KpiRow(mine);
        }
    }
}

static ImVec4 ProfessionColour(const std::string& spec) {
    std::string prof = SpecProfession(spec);
    if (prof == "Guardian") return ImVec4(0.45f, 0.76f, 0.85f, 1.0f);
    if (prof == "Warrior") return ImVec4(1.0f, 0.82f, 0.40f, 1.0f);
    if (prof == "Engineer") return ImVec4(0.82f, 0.61f, 0.35f, 1.0f);
    if (prof == "Ranger") return ImVec4(0.55f, 0.86f, 0.51f, 1.0f);
    if (prof == "Thief") return ImVec4(0.75f, 0.56f, 0.58f, 1.0f);
    if (prof == "Elementalist") return ImVec4(0.96f, 0.54f, 0.53f, 1.0f);
    if (prof == "Mesmer") return ImVec4(0.71f, 0.47f, 0.84f, 1.0f);
    if (prof == "Necromancer") return ImVec4(0.32f, 0.65f, 0.44f, 1.0f);
    if (prof == "Revenant") return ImVec4(0.82f, 0.43f, 0.35f, 1.0f);
    return MUTED;
}
static Texture_t* UiTexture(const char* key) { return SpecTexture(std::string("ui:") + key); }
static void UiIcon(const char* key, float size = 14.0f) {
    if (Texture_t* texture = UiTexture(key); texture && texture->Resource) { ImGui::Image((ImTextureID)texture->Resource, ImVec2(size, size)); ImGui::SameLine(0, 4); }
}

static const ImVec4 TEAM_RED{ 0.94f, 0.33f, 0.31f, 1.0f };
static const ImVec4 TEAM_BLUE{ 0.40f, 0.62f, 0.95f, 1.0f };
static const ImVec4 TEAM_GREEN{ 0.36f, 0.78f, 0.45f, 1.0f };
static ImVec4 TeamColour(const std::string& name) { return name == "Red" ? TEAM_RED : name == "Blue" ? TEAM_BLUE : name == "Green" ? TEAM_GREEN : MUTED; }
static const char* TeamLabel(const std::string& name) { return name == "Red" ? "Rouge" : name == "Blue" ? "Bleu" : name == "Green" ? "Vert" : name.c_str(); }

static void Bar(float fraction, const ImVec4& colour, float width) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float height = ImGui::GetTextLineHeight() * 0.6f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(40, 46, 54, 255), 3.0f);
    draw->AddRectFilled(pos, ImVec2(pos.x + width * std::max(0.0f, std::min(1.0f, fraction)), pos.y + height), ImGui::GetColorU32(colour), 3.0f);
    ImGui::Dummy(ImVec2(width, height));
}

// A specialization row like Fight Analysis draws it: count, icon, name, damage, a bar in the profession's colour.
static void SpecRow(const SpecCount& sc, uint64_t maxDamage, int maxCount, bool sortByDamage, bool shortNames, float width) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float height = ImGui::GetTextLineHeight() + 6.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec4 colour = ProfessionColour(sc.spec);
    ImVec4 wash = colour; wash.w = 0.28f;
    float fraction = sortByDamage ? (float)sc.damage / (float)std::max<uint64_t>(1, maxDamage) : (float)sc.count / (float)std::max(1, maxCount);
    draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(29, 35, 42, 255), 3.0f);
    draw->AddRectFilled(pos, ImVec2(pos.x + width * std::max(0.0f, std::min(1.0f, fraction)), pos.y + height), ImGui::GetColorU32(wash), 3.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 6.0f, pos.y + 3.0f));
    ImGui::TextColored(colour, "%d", sc.count);
    ImGui::SameLine(0, 8);
    SpecIcon(sc.spec, ImGui::GetTextLineHeight());
    ImGui::TextUnformatted(shortNames ? SpecShort(sc.spec).c_str() : sc.spec.c_str());
    ImGui::SameLine(0, 8);
    ImGui::TextColored(MUTED, "(%s)", Thousands((double)sc.damage).c_str());
    if (sc.deaths || sc.downs) { ImGui::SameLine(0, 8); ImGui::TextColored(RED, "%s", Fallen(sc.deaths, sc.downs).c_str()); }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + 3.0f));
}

static int SelectedTeam = 0;

// Called with g_fights.mutex and g_state.mutex held: the fights read in game, newest first.
static void RenderCombats() {
    Settings& st = g_state.settings;
    const ParsedFight* shown = ShownFight(false);
    if (!shown) { ArcdpsLine(true); ImGui::TextColored(MUTED, "%s", g_fights.status.c_str()); Wrapped("Les combats apparaissent ici dès qu'arcdps a écrit leur log, sans attendre l'envoi."); return; }
    ArcdpsLine(false);
    const ParsedFight& f = *shown;
    float width = ImGui::GetContentRegionAvail().x;
    if (f.wvw && f.teams.empty() && f.eventsRead) Callout(GOLD, "Log McM sans équipe : " + std::to_string(f.agentsRead) + " agents, " + std::to_string(f.playersRead) + " joueurs (" + std::to_string(f.playersSeen) + " vus, " + std::to_string(f.playersWithTeam) + " avec équipe), " + std::to_string(f.coloursKnown) + " couleur(s), " + std::to_string(f.eventsRead) + " événements" + (f.hasPov ? "" : ", pas de point de vue") + ". Envoie ce fichier à Mathieu.");
    ImGui::TextColored(MUTED, "%s", f.file.c_str());
    ImGui::SameLine();
    ImGui::Text("(%s)", Clock((int)f.durationMs).c_str());
    ImGui::SameLine(width - 140.0f);
    if (ImGui::SmallButton("Historique")) ImGui::OpenPopup("forge-history");
    ImGui::SameLine();
    if (ImGui::SmallButton("Affichage")) ImGui::OpenPopup("forge-display");
    if (ImGui::BeginPopup("forge-history")) {
        for (size_t index = 0; index < g_fights.fights.size(); index++) {
            const ParsedFight& h = g_fights.fights[index];
            std::string label = (h.wvw ? "McM" : (h.boss.empty() ? "PvE" : h.boss)) + " · " + Clock((int)h.durationMs) + (h.success ? " · kill" : "") + "##" + std::to_string(index);
            if (ImGui::Selectable(label.c_str(), (int)index == g_fights.selected)) g_fights.selected = (int)index;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("forge-display")) {
        bool changed = false;
        changed |= ImGui::Checkbox("Trier par dégâts", &st.sortByDamage);
        changed |= ImGui::Checkbox("Escouade seulement", &st.squadOnly);
        changed |= ImGui::Checkbox("Noms courts", &st.shortNames);
        changed |= ImGui::Checkbox("Barre des effectifs", &st.showStrip);
        changed |= ImGui::Checkbox("Panneau du combat", &st.showWidget);
        changed |= ImGui::Checkbox("Masquer le widget en combat", &st.hideInCombat);
        if (changed) SaveSettingsLocked(SettingsPath);
        ImGui::EndPopup();
    }
    ImGui::Separator();
    if (f.wvw) {
        if (f.teams.empty()) { ImGui::TextColored(MUTED, "%s", "Aucune équipe reconnue dans ce log."); return; }
        if (SelectedTeam >= (int)f.teams.size()) SelectedTeam = 0;
        for (size_t index = 0; index < f.teams.size(); index++) {
            if (index) ImGui::SameLine();
            const TeamSummary& t = f.teams[index];
            ImVec4 colour = TeamColour(t.name);
            bool active = (int)index == SelectedTeam;
            ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(colour.x, colour.y, colour.z, 0.35f) : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, colour);
            if (ImGui::Button((std::string(TeamLabel(t.name)) + (t.pov ? " (nous)" : "")).c_str())) SelectedTeam = (int)index;
            ImGui::PopStyleColor(2);
        }
        const TeamSummary& t = f.teams[SelectedTeam];
        bool squadView = t.pov && st.squadOnly;
        std::vector<SpecCount> rows;
        int players = t.players, deaths = t.deaths, downs = t.downs, kills = t.kills;
        uint64_t damage = t.damage;
        if (squadView) {
            std::map<std::string, SpecCount> bySpec;
            players = 0; deaths = 0; downs = 0; damage = 0; kills = 0;
            for (const auto& line : f.squad) { auto& sc = bySpec[line.spec]; sc.spec = line.spec; sc.count++; sc.damage += line.damage; sc.deaths += line.deaths; sc.downs += line.downs; players++; deaths += line.deaths; downs += line.downs; damage += line.damage; kills += line.kills; }
            for (auto& [spec, sc] : bySpec) rows.push_back(sc);
        } else rows = t.specs;
        UiIcon("squad"); ImGui::Text("Total : %d", players);
        ImGui::SameLine(0, 14); UiIcon("kill"); ImGui::Text("Kills : %d", kills);
        ImGui::SameLine(0, 14); UiIcon("death"); ImGui::TextColored(RED, "Morts : %d", deaths);
        ImGui::SameLine(0, 14); UiIcon("downed"); ImGui::TextColored(GOLD, "À terre : %d", downs);
        UiIcon("damage"); ImGui::Text("Dégâts : %s", Thousands((double)damage).c_str());
        ImGui::Separator();
        std::sort(rows.begin(), rows.end(), [&](const SpecCount& a, const SpecCount& b) { return st.sortByDamage ? a.damage > b.damage : (a.count != b.count ? a.count > b.count : a.damage > b.damage); });
        uint64_t maxDamage = 1; int maxCount = 1;
        for (const auto& sc : rows) { maxDamage = std::max(maxDamage, sc.damage); maxCount = std::max(maxCount, sc.count); }
        for (const auto& sc : rows) SpecRow(sc, maxDamage, maxCount, st.sortByDamage, st.shortNames, width);
    } else {
        if (NexusLink && NexusLink->FontBig) ImGui::PushFont((ImFont*)NexusLink->FontBig);
        ImGui::TextUnformatted(f.boss.c_str());
        if (NexusLink && NexusLink->FontBig) ImGui::PopFont();
        ImGui::SameLine();
        if (f.success) ImGui::TextColored(GREEN, "kill en %s", Clock((int)f.durationMs).c_str());
        else if (f.hpLeft >= 0) ImGui::TextColored(RED, "wipe à %.1f %% (%s)", f.hpLeft, Clock((int)f.durationMs).c_str());
        else ImGui::TextColored(RED, "wipe (%s)", Clock((int)f.durationMs).c_str());
        double seconds = std::max(1.0, f.durationMs / 1000.0);
        if (f.hasMe) {
            int rank = 0;
            for (size_t i = 0; i < f.squad.size(); i++) if (f.squad[i].pov) rank = (int)i + 1;
            std::vector<Kpi> tiles;
            tiles.push_back({ Thousands(f.me.damage / seconds), "Ton DPS", rank ? std::to_string(rank) + "e sur " + std::to_string(f.squad.size()) : "", SKY });
            tiles.push_back({ std::to_string(f.me.deaths), "Morts", std::to_string(f.me.downs) + " à terre", f.me.deaths ? RED : GREEN });
            int squadDeaths = 0, squadDowns = 0;
            for (const auto& line : f.squad) { squadDeaths += line.deaths; squadDowns += line.downs; }
            tiles.push_back({ std::to_string(squadDeaths), "Morts escouade", std::to_string(squadDowns) + " à terre", squadDeaths ? GOLD : GREEN });
            KpiRow(tiles);
        }
        if (!f.squad.empty()) {
            ImGui::TextColored(GOLD, "%s", "Escouade · dégâts sur le boss");
            uint64_t top = std::max<uint64_t>(1, f.squad.front().damage);
            int shown = 0;
            for (const auto& line : f.squad) {
                if (shown++ >= 12) { ImGui::TextColored(MUTED, "... et %d autres", (int)f.squad.size() - 12); break; }
                ImVec2 pos = ImGui::GetCursorScreenPos();
                float height = ImGui::GetTextLineHeight() + 6.0f;
                ImVec4 colour = line.pov ? SKY : ProfessionColour(line.spec);
                ImVec4 wash = colour; wash.w = 0.28f;
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(29, 35, 42, 255), 3.0f);
                draw->AddRectFilled(pos, ImVec2(pos.x + width * (float)line.damage / (float)top, pos.y + height), ImGui::GetColorU32(wash), 3.0f);
                ImGui::SetCursorScreenPos(ImVec2(pos.x + 6.0f, pos.y + 3.0f));
                SpecIcon(line.spec, ImGui::GetTextLineHeight());
                ImGui::TextColored(line.pov ? SKY : ImVec4(0.93f, 0.94f, 0.95f, 1.0f), "%s", line.name.c_str());
                ImGui::SameLine(0, 8);
                ImGui::TextColored(MUTED, "%s /s", Thousands(line.damage / seconds).c_str());
                if (line.deaths || line.downs) { ImGui::SameLine(0, 8); ImGui::TextColored(RED, "%s", Fallen(line.deaths, line.downs).c_str()); }
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + 3.0f));
            }
        }
    }
}

// The strip, as WvW Fight Analysis draws it: a dark square with the squad icon, then the teams' counts as coloured
// segments with the number in white (PvE: one segment with the boss, the outcome and the recorder's DPS). A click
// opens or closes the compact panel.
static void RenderStrip() {
    ArcdpsNoteCombat(InCombat());
    LiveRefresh();
    Settings st;
    { std::lock_guard<std::mutex> lock(g_state.mutex); st = g_state.settings; }
    if (!st.showStrip || (st.hideInCombat && InCombat())) return;
    std::lock_guard<std::mutex> flock(g_fights.mutex);
    if (NexusLink && NexusLink->Font) ImGui::PushFont((ImFont*)NexusLink->Font);
    const float scale = st.fontScale;
    const float height = 26.0f * scale, iconBox = 34.0f * scale, barWidth = 320.0f * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(10, 10));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::SetNextWindowPos(ImVec2(st.stripX, st.stripY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(iconBox + barWidth, height));
    if (ImGui::Begin("Forge strip", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImU32 dark = IM_COL32(24, 24, 28, 235), outline = IM_COL32(60, 60, 66, 255), white = IM_COL32(245, 245, 245, 255);
        // Icon box.
        draw->AddRectFilled(pos, ImVec2(pos.x + iconBox, pos.y + height), dark);
        draw->AddRect(pos, ImVec2(pos.x + iconBox + barWidth, pos.y + height), outline);
        if (Texture_t* icon = UiTexture("squad"); icon && icon->Resource) draw->AddImage((ImTextureID)icon->Resource, ImVec2(pos.x + (iconBox - 16 * scale) / 2, pos.y + (height - 16 * scale) / 2), ImVec2(pos.x + (iconBox + 16 * scale) / 2, pos.y + (height + 16 * scale) / 2));
        float x = pos.x + iconBox;
        auto segment = [&](float w, ImU32 colour, const std::string& text) {
            draw->AddRectFilled(ImVec2(x, pos.y), ImVec2(x + w, pos.y + height), colour);
            ImVec2 size = ImGui::CalcTextSize(text.c_str());
            if (w >= size.x + 8) draw->AddText(ImVec2(x + (w - size.x) / 2, pos.y + (height - size.y) / 2), white, text.c_str());
            x += w;
        };
        const ParsedFight* shown = ShownFight(true);
        if (!shown) {
            const char* warning = ArcdpsShort();
            segment(barWidth, warning ? IM_COL32(120, 60, 30, 235) : IM_COL32(40, 42, 48, 235), warning ? warning : "Forge · en attente d'un combat");
        } else {
            const ParsedFight& f = *shown;
            if (f.wvw && !f.teams.empty()) {
                float total = 0;
                for (const auto& t : f.teams) total += t.players;
                for (const auto& t : f.teams) {
                    ImVec4 c = TeamColour(t.name);
                    ImU32 colour = t.name == "Red" ? IM_COL32(211, 74, 58, 235) : t.name == "Blue" ? IM_COL32(58, 166, 217, 235) : t.name == "Green" ? IM_COL32(141, 197, 63, 235) : ImGui::GetColorU32(c);
                    segment(total > 0 ? barWidth * t.players / total : 0, colour, std::to_string(t.players));
                }
            } else {
                double seconds = std::max(1.0, f.durationMs / 1000.0);
                std::string text = f.boss + (f.success ? " · kill " : f.hpLeft >= 0 ? " · wipe " + std::to_string((int)(f.hpLeft + 0.5)) + " % " : " · wipe ") + Clock((int)f.durationMs);
                if (f.hasMe) {
                    int rank = 0;
                    for (size_t i = 0; i < f.squad.size(); i++) if (f.squad[i].pov) rank = (int)i + 1;
                    text += " · " + Thousands(f.me.damage / seconds) + " DPS" + (rank ? " (" + std::to_string(rank) + "e)" : "");
                }
                segment(barWidth, f.success ? IM_COL32(52, 140, 88, 235) : IM_COL32(150, 52, 48, 235), text);
            }
        }
        // The icon is the handle: drag it to move the strip. The bar itself opens and closes the panel.
        static bool dragging = false;
        bool onIcon = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + iconBox, pos.y + height));
        if (onIcon && ImGui::IsMouseClicked(0)) dragging = true;
        if (dragging) {
            if (ImGui::IsMouseDown(0)) {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                ImVec2 at = ImGui::GetWindowPos();
                ImGui::SetWindowPos(ImVec2(at.x + delta.x, at.y + delta.y), ImGuiCond_Always);
            } else {
                dragging = false;
                ImVec2 at = ImGui::GetWindowPos();
                std::lock_guard<std::mutex> lock(g_state.mutex);
                g_state.settings.stripX = at.x;
                g_state.settings.stripY = at.y;
                SaveSettingsLocked(SettingsPath);
            }
        } else if (onIcon) ImGui::SetTooltip("%s", "Glisser pour déplacer la barre");
        ImGui::SetCursorScreenPos(ImVec2(pos.x + iconBox, pos.y));
        if (ImGui::InvisibleButton("forge-strip", ImVec2(barWidth, height))) {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.settings.showWidget = !g_state.settings.showWidget;
            SaveSettingsLocked(SettingsPath);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Clic : ouvrir / fermer le panneau du combat");
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    if (NexusLink && NexusLink->Font) ImGui::PopFont();
}

static int WidgetTeam = 0;

// The right-click menu of the panel: history, what to show, style, close. Called with both mutexes held.
static void PanelMenu(Settings& st) {
    if (!ImGui::BeginPopupContextWindow("forge-panel-menu")) return;
    bool changed = false;
    if (ImGui::BeginMenu("Historique")) {
        for (size_t index = 0; index < g_fights.fights.size(); index++) {
            const ParsedFight& h = g_fights.fights[index];
            std::string label = (h.wvw ? "McM" : (h.boss.empty() ? "PvE" : h.boss)) + " · " + Clock((int)h.durationMs) + (h.success ? " · kill" : "") + "##" + std::to_string(index);
            if (ImGui::MenuItem(label.c_str(), nullptr, (int)index == g_fights.selected)) g_fights.selected = (int)index;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Affichage")) {
        changed |= ImGui::MenuItem("Trier par dégâts de classe", nullptr, &st.sortByDamage);
        changed |= ImGui::MenuItem("Alliés hors escouade : afficher", nullptr, &st.panelAllies);
        changed |= ImGui::MenuItem("Alliés hors escouade : compter dans le total", nullptr, &st.panelAlliesTotal);
        ImGui::Separator();
        changed |= ImGui::MenuItem("Barres par classe", nullptr, &st.panelBars);
        changed |= ImGui::MenuItem("Noms courts", nullptr, &st.shortNames);
        changed |= ImGui::MenuItem("Noms des classes", nullptr, &st.panelNames);
        changed |= ImGui::MenuItem("Icônes des classes", nullptr, &st.panelIcons);
        changed |= ImGui::MenuItem("Dégâts par classe", nullptr, &st.panelClassDamage);
        ImGui::Separator();
        changed |= ImGui::MenuItem("Total joueurs", nullptr, &st.panelTotal);
        changed |= ImGui::MenuItem("K/D de l'équipe", nullptr, &st.panelKd);
        changed |= ImGui::MenuItem("Morts", nullptr, &st.panelDeaths);
        changed |= ImGui::MenuItem("À terre", nullptr, &st.panelDowns);
        changed |= ImGui::MenuItem("Dégâts", nullptr, &st.panelDamage);
        changed |= ImGui::MenuItem("Dégâts directs", nullptr, &st.panelStrike);
        changed |= ImGui::MenuItem("Dégâts d'altération", nullptr, &st.panelCondi);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Style")) {
        changed |= ImGui::SliderFloat("Taille du texte", &st.fontScale, 0.8f, 1.6f, "%.1f");
        changed |= ImGui::SliderFloat("Opacité", &st.panelAlpha, 0.3f, 1.0f, "%.2f");
        changed |= ImGui::MenuItem("Masquer en combat", nullptr, &st.hideInCombat);
        changed |= ImGui::MenuItem("Barre des effectifs", nullptr, &st.showStrip);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Fenêtre Forge", nullptr, WindowVisible)) WindowVisible = !WindowVisible;
    bool placing = !st.alertsLocked;
    if (ImGui::MenuItem("Déplacer les barres et les auras", nullptr, &placing)) { st.alertsLocked = !placing; changed = true; }
    if (ImGui::MenuItem("Fermer le panneau")) { st.showWidget = false; changed = true; }
    if (changed) SaveSettingsLocked(SettingsPath);
    ImGui::EndPopup();
}

// The panel, as WvW Fight Analysis' compact window: title, log name and duration, one tab per team, totals with
// icons, then a row per specialization with its count, icon, name, damage and a bar in the profession's colour.
// PvE: boss and outcome, the recorder's DPS, then the squad's damage rows. Right-click: the menu above.
static void RenderWidget() {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    Settings& st = g_state.settings;
    if (!st.showWidget || (st.hideInCombat && InCombat())) return;
    std::lock_guard<std::mutex> flock(g_fights.mutex);
    if (NexusLink && NexusLink->Font) ImGui::PushFont((ImFont*)NexusLink->Font);
    const float scale = st.fontScale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * scale, 6 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * scale, 3 * scale));
    ImGui::SetNextWindowBgAlpha(st.panelAlpha);
    ImGui::SetNextWindowPos(ImVec2(st.panelX, st.panelY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(250.0f * scale, 0.0f), ImGuiCond_Always);
    if (ImGui::Begin("Forge widget", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse)) {
        ImGui::SetWindowFontScale(scale);
        PanelMenu(st);
        float width = ImGui::GetContentRegionAvail().x;
        ImGui::TextColored(ImVec4(0.93f, 0.94f, 0.95f, 1.0f), "%s", "Forge · Combat");
        ImGui::SameLine(width - 14 * scale);
        if (ImGui::SmallButton("×")) { st.showWidget = false; SaveSettingsLocked(SettingsPath); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Fermer (la barre des effectifs le rouvre)");
        const ParsedFight* shown = ShownFight(false);
        if (!shown) {
            const char* warning = ArcdpsShort();
            if (warning) ImGui::TextColored(GOLD, "%s", warning);
            ImGui::TextColored(MUTED, "%s", "En attente d'un combat");
        } else {
            const ParsedFight& f = *shown;
            ImGui::TextColored(MUTED, "%s (%s)", f.file.size() > 22 ? f.file.substr(0, 22).c_str() : f.file.c_str(), Clock((int)f.durationMs).c_str());
            if (f.wvw && !f.teams.empty()) {
                if (WidgetTeam >= (int)f.teams.size()) WidgetTeam = 0;
                for (size_t index = 0; index < f.teams.size(); index++) {
                    if (index) ImGui::SameLine(0, 2);
                    ImVec4 colour = TeamColour(f.teams[index].name);
                    bool active = (int)index == WidgetTeam;
                    ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(colour.x, colour.y, colour.z, 0.45f) : ImVec4(0.16f, 0.17f, 0.20f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(1, 1, 1, 1) : colour);
                    if (ImGui::SmallButton(TeamLabel(f.teams[index].name))) WidgetTeam = (int)index;
                    ImGui::PopStyleColor(2);
                }
                ImGui::Separator();
                const TeamSummary& t = f.teams[WidgetTeam];
                // Own team: the squad alone, or with the allies around it, for the rows and for the totals as set.
                std::vector<SpecCount> squadRows;
                int squadPlayers = 0, squadDeaths = 0, squadDowns = 0, squadKills = 0;
                uint64_t squadDamage = 0, squadStrike = 0, squadCondi = 0;
                if (t.pov) {
                    std::map<std::string, SpecCount> bySpec;
                    for (const auto& line : f.squad) { auto& sc = bySpec[line.spec]; sc.spec = line.spec; sc.count++; sc.damage += line.damage; sc.strike += line.strike; sc.condi += line.condi; sc.deaths += line.deaths; sc.downs += line.downs; squadPlayers++; squadDeaths += line.deaths; squadDowns += line.downs; squadDamage += line.damage; squadStrike += line.strike; squadCondi += line.condi; squadKills += line.kills; }
                    for (auto& [spec, sc] : bySpec) squadRows.push_back(sc);
                }
                bool alliesInRows = !t.pov || st.panelAllies, alliesInTotals = !t.pov || st.panelAlliesTotal;
                std::vector<SpecCount> rows = alliesInRows ? t.specs : squadRows;
                int players = alliesInTotals ? t.players : squadPlayers, deaths = alliesInTotals ? t.deaths : squadDeaths, downs = alliesInTotals ? t.downs : squadDowns, kills = alliesInTotals ? t.kills : squadKills;
                uint64_t damage = alliesInTotals ? t.damage : squadDamage, strike = alliesInTotals ? t.strike : squadStrike, condi = alliesInTotals ? t.condi : squadCondi;
                float lh = ImGui::GetTextLineHeight();
                // Own team with the allies in: each total also split squad / allies.
                bool split = t.pov && alliesInTotals && st.panelAllies && t.players > squadPlayers;
                auto part = [&](int squad, int all) { return split ? " (" + std::to_string(squad) + " + " + std::to_string(all - squad) + ")" : std::string(); };
                if (st.panelTotal) { UiIcon("squad", lh); ImGui::Text("Total : %d%s", players, part(squadPlayers, players).c_str()); if (split && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "escouade + alliés hors escouade"); }
                if (st.panelKd) { UiIcon("kill", lh); ImGui::Text("K/D : %.1f", deaths ? (double)kills / deaths : (double)kills); }
                if (st.panelDeaths) { UiIcon("death", lh); ImGui::Text("Morts : %d%s", deaths, part(squadDeaths, deaths).c_str()); }
                if (st.panelDowns) { UiIcon("downed", lh); ImGui::Text("À terre : %d%s", downs, part(squadDowns, downs).c_str()); }
                auto money = [&](uint64_t all, uint64_t squad) { return Thousands((double)all) + (split ? " (" + Thousands((double)squad) + " + " + Thousands((double)(all - squad)) + ")" : ""); };
                if (st.panelDamage) { UiIcon("damage", lh); ImGui::Text("Dégâts : %s", money(damage, squadDamage).c_str()); }
                if (st.panelStrike) { UiIcon("damage", lh); ImGui::Text("Directs : %s", money(strike, squadStrike).c_str()); }
                if (st.panelCondi) { UiIcon("damage", lh); ImGui::Text("Altérations : %s", money(condi, squadCondi).c_str()); }
                ImGui::Separator();
                std::sort(rows.begin(), rows.end(), [&](const SpecCount& a, const SpecCount& b) { return st.sortByDamage ? a.damage > b.damage : (a.count != b.count ? a.count > b.count : a.damage > b.damage); });
                uint64_t maxDamage = 1; int maxCount = 1;
                for (const auto& sc : rows) { maxDamage = std::max(maxDamage, sc.damage); maxCount = std::max(maxCount, sc.count); }
                for (const auto& sc : rows) {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    float height = lh + 4 * scale;
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    ImVec4 colour = ProfessionColour(sc.spec);
                    float fraction = st.sortByDamage ? (float)sc.damage / (float)maxDamage : (float)sc.count / (float)maxCount;
                    if (st.panelBars) draw->AddRectFilled(pos, ImVec2(pos.x + width * std::max(0.05f, std::min(1.0f, fraction)), pos.y + height), ImGui::GetColorU32(ImVec4(colour.x * 0.55f, colour.y * 0.55f, colour.z * 0.55f, 0.9f)), 2.0f);
                    ImGui::SetCursorScreenPos(ImVec2(pos.x + 4 * scale, pos.y + 2 * scale));
                    ImGui::Text("%d", sc.count);
                    ImGui::SameLine(0, 5 * scale);
                    if (st.panelIcons) SpecIcon(sc.spec, lh);
                    std::string text;
                    if (st.panelNames) text = st.shortNames ? SpecShort(sc.spec) : sc.spec;
                    if (st.panelClassDamage) text += (text.empty() ? "" : " ") + std::string("(") + Thousands((double)sc.damage) + ")";
                    if (!text.empty()) ImGui::TextUnformatted(text.c_str());
                    else ImGui::Dummy(ImVec2(1, lh));
                    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + 2 * scale));
                }
            } else {
                if (f.success) ImGui::TextColored(GREEN, "%s · kill", f.boss.c_str());
                else if (f.hpLeft >= 0) ImGui::TextColored(RED, "%s · wipe %.0f %%", f.boss.c_str(), f.hpLeft);
                else ImGui::TextColored(RED, "%s · wipe", f.boss.c_str());
                double seconds = std::max(1.0, f.durationMs / 1000.0);
                if (f.hasMe) {
                    int rank = 0;
                    for (size_t i = 0; i < f.squad.size(); i++) if (f.squad[i].pov) rank = (int)i + 1;
                    ImGui::TextColored(SKY, "Toi : %s DPS%s", Thousands(f.me.damage / seconds).c_str(), rank ? (" · " + std::to_string(rank) + "e/" + std::to_string(f.squad.size())).c_str() : "");
                    if (f.me.deaths || f.me.downs) { ImGui::SameLine(0, 6); ImGui::TextColored(RED, "%s", Fallen(f.me.deaths, f.me.downs).c_str()); }
                }
                ImGui::Separator();
                uint64_t top = 1;
                for (const auto& line : f.squad) top = std::max(top, line.damage);
                int shown = 0;
                for (const auto& line : f.squad) {
                    if (shown++ >= 10) break;
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    float height = ImGui::GetTextLineHeight() + 4 * scale;
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    ImVec4 colour = line.pov ? SKY : ProfessionColour(line.spec);
                    if (st.panelBars) draw->AddRectFilled(pos, ImVec2(pos.x + width * std::max(0.05f, (float)line.damage / (float)top), pos.y + height), ImGui::GetColorU32(ImVec4(colour.x * 0.55f, colour.y * 0.55f, colour.z * 0.55f, 0.9f)), 2.0f);
                    ImGui::SetCursorScreenPos(ImVec2(pos.x + 4 * scale, pos.y + 2 * scale));
                    if (st.panelIcons) SpecIcon(line.spec, ImGui::GetTextLineHeight());
                    ImGui::Text("%s (%s)", line.name.size() > 14 ? line.name.substr(0, 14).c_str() : line.name.c_str(), Thousands(line.damage / seconds).c_str());
                    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + height + 2 * scale));
                }
            }
        }
        ImGui::SetWindowFontScale(1.0f);
        ImVec2 at = ImGui::GetWindowPos();
        if ((at.x != st.panelX || at.y != st.panelY) && !ImGui::IsMouseDown(0)) { st.panelX = at.x; st.panelY = at.y; SaveSettingsLocked(SettingsPath); }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    if (NexusLink && NexusLink->Font) ImGui::PopFont();
}

static void RenderWindow() {
    bool show = WindowVisible;
    if (!show) return;
    if (NexusLink && NexusLink->Font) ImGui::PushFont((ImFont*)NexusLink->Font);
    ImGui::SetNextWindowSize(ImVec2(520, 620), ImGuiCond_FirstUseEver);
    std::string title = std::string("Forge ") + VersionText() + "###" + WINDOW;
    if (ImGui::Begin(title.c_str(), &show, ImGuiWindowFlags_NoCollapse)) {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        ImGui::SetWindowFontScale(g_state.settings.fontScale);
        if (!g_state.hasNight) {
            ImGui::TextColored(g_state.tokenOk ? MUTED : RED, "%s", g_state.status.c_str());
            if (!g_state.tokenOk) Wrapped("Options Nexus > Forge : colle ton token Forge Uploader (Forge > Mon suivi > Réglages > Forge Uploader).");
            else { ImGui::Separator(); if (ImGui::BeginTabBar("forge-tabs-nonight")) { if (ImGui::BeginTabItem("Combats")) { std::lock_guard<std::mutex> flock(g_fights.mutex); RenderCombats(); ImGui::EndTabItem(); } if (ImGui::BeginTabItem("Forge")) { RenderStats(); ImGui::EndTabItem(); } if (ImGui::BeginTabItem("Alertes")) { AlertsTab(); ImGui::EndTabItem(); } ImGui::EndTabBar(); } }
        } else {
            const LiveNight& n = g_state.night;
            // Header: the roster (a combo when the member plays in several), day, phase.
            if (n.rosters.size() > 1) {
                ImGui::SetNextItemWidth(220);
                if (ImGui::BeginCombo("##roster", n.rosterName.c_str())) {
                    for (const auto& roster : n.rosters) {
                        std::string text = roster.name + (roster.live ? "  (en direct)" : roster.tonight ? "  · ce soir" : "");
                        if (ImGui::Selectable(text.c_str(), roster.id == n.rosterId) && roster.id != n.rosterId) {
                            g_state.settings.rosterId = roster.id;
                            SaveSettingsLocked(SettingsPath);
                            PollNow = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextColored(MUTED, "%s", n.day.c_str());
            } else ImGui::TextColored(MUTED, "%s · %s", n.rosterName.c_str(), n.day.c_str());
            ImGui::SameLine();
            if (n.phase == "live") ImGui::TextColored(RED, "%s", "EN DIRECT");
            else if (n.phase == "ended") ImGui::TextColored(GREEN, "%s", "Soirée terminée");
            else ImGui::TextColored(MUTED, "%s", "En attente du lancement");
            // Boss strip; leads jump to a boss by clicking it.
            for (size_t index = 0; index < n.bosses.size(); index++) {
                const auto& boss = n.bosses[index];
                if (index) ImGui::SameLine();
                ImVec4 color = boss.killed ? GREEN : boss.on ? RED : MUTED;
                std::string text = boss.on ? "» " + boss.label : boss.label + (boss.killed ? " OK" : "");
                if (n.canPlan && n.phase == "live" && !boss.on) {
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    if (ImGui::SmallButton(text.c_str())) { std::string o = boss.killed ? "undo" : "go", b = boss.id; std::thread([o, b]() { SendOp(o, b); }).detach(); }
                    ImGui::PopStyleColor();
                } else ImGui::TextColored(color, "%s", text.c_str());
            }
            ImGui::Separator();
            if (n.canPlan) {
                if (n.phase != "live") LeadButton(n.phase == "ended" ? "Reprendre le direct" : "Lancer le direct", "start");
                else {
                    LeadButton("Kill", "kill"); ImGui::SameLine();
                    if (n.hasNext) { LeadButton("Passer", "skip"); ImGui::SameLine(); }
                    LeadButton("Finir la soirée", "end");
                }
                if (!g_state.error.empty()) ImGui::TextColored(RED, "%s", g_state.error.c_str());
                ImGui::Separator();
            }
            if (n.phase == "ended") {
                ImGui::Text("%d boss sur %d.", n.killed, (int)n.bosses.size());
                ImGui::Separator();
                { std::lock_guard<std::mutex> flock(g_fights.mutex); RenderCombats(); }
                ImGui::Separator();
                RenderStats();
                if (ImGui::CollapsingHeader("Alertes")) AlertsTab();
            } else if (!n.hasCurrent) {
                ImGui::TextColored(MUTED, "%s", "Aucun boss prévu pour cette soirée.");
                if (ImGui::CollapsingHeader("Alertes")) AlertsTab();
            } else if (ImGui::BeginTabBar("forge-tabs")) {
                const LiveBoss& cur = n.current;
                if (ImGui::BeginTabItem("En direct")) {
                    ImGui::TextColored(MUTED, "%s", n.phase == "live" ? "Boss en cours" : "Premier boss");
                    if (NexusLink && NexusLink->FontBig) ImGui::PushFont((ImFont*)NexusLink->FontBig);
                    ImGui::TextUnformatted(cur.label.c_str());
                    if (NexusLink && NexusLink->FontBig) ImGui::PopFont();
                    ImGui::TextColored(MUTED, "%s", "Ta place");
                    ImGui::SameLine();
                    ImGui::TextColored(SKY, "%s", cur.hasPlace ? cur.myPlace.c_str() : "-");
                    ImGui::Spacing();
                    for (const auto& goal : cur.goals) {
                        ImGui::TextColored(goal.met ? GREEN : GOLD, "%s %s", goal.met ? "[x]" : "[ ]", goal.label.c_str());
                        ImGui::TextColored(MUTED, "   %s", goal.state.c_str());
                    }
                    if (!cur.goals.empty()) ImGui::Spacing();
                    if (cur.hasGuide) {
                        ImGui::TextColored(GOLD, "%s", "L'essentiel");
                        for (const auto& line : cur.essentials) Bullet(line);
                        ImGui::Spacing();
                        ImGui::TextColored(GOLD, "%s", "Mécaniques clés");
                        RenderMechanics(cur, false);
                    } else ImGui::TextColored(MUTED, "%s", "Pas de guide pour ce boss.");
                    if (n.hasNext) {
                        ImGui::Separator();
                        ImGui::TextColored(MUTED, "%s", "Ensuite");
                        ImGui::SameLine();
                        ImGui::TextUnformatted(n.next.label.c_str());
                        if (n.next.hasPlace) { ImGui::SameLine(); ImGui::TextColored(SKY, "· %s", n.next.myPlace.c_str()); }
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Strats")) {
                    if (!cur.hasGuide) ImGui::TextColored(MUTED, "%s", "Pas de guide pour ce boss.");
                    else {
                        if (!cur.squad.empty()) {
                            ImGui::TextColored(GOLD, "%s", "Compo demandée");
                            for (const auto& line : cur.squad) Bullet(line);
                            ImGui::Spacing();
                        }
                        for (const auto& section : cur.sections) {
                            if (ImGui::CollapsingHeader(section.title.c_str())) {
                                for (const auto& line : section.body) Wrapped(line);
                                for (const auto& tip : section.tips) Bullet(tip);
                            }
                        }
                        ImGui::Spacing();
                        if (ImGui::CollapsingHeader("Toutes les mécaniques", ImGuiTreeNodeFlags_DefaultOpen)) RenderMechanics(cur, true);
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Compo")) {
                    ImGui::TextColored(GOLD, "%s", cur.label.c_str());
                    RenderSlots(cur);
                    if (n.hasNext) {
                        ImGui::Spacing();
                        ImGui::TextColored(GOLD, "Ensuite : %s", n.next.label.c_str());
                        if (!n.next.squad.empty()) for (const auto& line : n.next.squad) Bullet(line);
                        RenderSlots(n.next);
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Combats")) { std::lock_guard<std::mutex> flock(g_fights.mutex); RenderCombats(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Stats Forge")) { RenderStats(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Alertes")) { AlertsTab(); ImGui::EndTabItem(); }
                if (n.hasNext && ImGui::BeginTabItem("Prochain boss")) {
                    ImGui::TextUnformatted(n.next.label.c_str());
                    if (n.next.hasGuide) {
                        for (const auto& line : n.next.essentials) Bullet(line);
                        ImGui::Spacing();
                        RenderMechanics(n.next, false);
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
    if (NexusLink && NexusLink->Font) ImGui::PopFont();
    WindowVisible = show;
}

static char TokenBuffer[128]{};
static char UrlBuffer[256]{};
static char LogsBuffer[512]{};
static std::string Narrow(const std::wstring& text) { if (text.empty()) return ""; int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr); std::string out(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n, nullptr, nullptr); return out; }
static std::wstring Widen(const std::string& text) { if (text.empty()) return L""; int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0); std::wstring out(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], n); return out; }

static void RenderOptions() {
    ImGui::TextColored(MUTED, "Forge addon %s", VersionText().c_str());
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (!TokenBuffer[0] && !g_state.settings.token.empty()) snprintf(TokenBuffer, sizeof(TokenBuffer), "%s", g_state.settings.token.c_str());
        if (!UrlBuffer[0]) snprintf(UrlBuffer, sizeof(UrlBuffer), "%s", g_state.settings.forgeUrl.c_str());
    }
    ImGui::TextWrapped("%s", "Token Forge Uploader : dans Forge, Mon suivi > Réglages > Forge Uploader > Créer mon token. Le même que pour l'uploader.");
    ImGui::InputText("Token Forge", TokenBuffer, sizeof(TokenBuffer), ImGuiInputTextFlags_Password);
    ImGui::InputText("Adresse de Forge", UrlBuffer, sizeof(UrlBuffer));
    { std::lock_guard<std::mutex> flock(g_fights.mutex); if (!LogsBuffer[0]) snprintf(LogsBuffer, sizeof(LogsBuffer), "%s", Narrow(g_fights.logsDir).c_str()); }
    ImGui::InputText("Dossier des logs arcdps", LogsBuffer, sizeof(LogsBuffer));
    { std::lock_guard<std::mutex> lock(g_state.mutex); bool changed = ImGui::Checkbox("Barre des effectifs (un clic ouvre le panneau)", &g_state.settings.showStrip); changed |= ImGui::Checkbox("Panneau du combat", &g_state.settings.showWidget); changed |= ImGui::Checkbox("Masquer le widget en combat", &g_state.settings.hideInCombat); if (changed) SaveSettingsLocked(SettingsPath); }
    float scale;
    { std::lock_guard<std::mutex> lock(g_state.mutex); scale = g_state.settings.fontScale; }
    if (ImGui::SliderFloat("Taille du texte", &scale, 0.8f, 1.6f, "%.1f")) { std::lock_guard<std::mutex> lock(g_state.mutex); g_state.settings.fontScale = scale; }
    if (ImGui::Button("Enregistrer et vérifier")) {
        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.settings.token = TokenBuffer;
            g_state.settings.forgeUrl = UrlBuffer;
            g_state.settings.logsDir = LogsBuffer;
            while (!g_state.settings.forgeUrl.empty() && g_state.settings.forgeUrl.back() == '/') g_state.settings.forgeUrl.pop_back();
        }
        SaveSettings(SettingsPath);
        { std::lock_guard<std::mutex> flock(g_fights.mutex); g_fights.logsDir = Widen(LogsBuffer); }
        PollNow = true;
    }
    ArcdpsLine(true);
    std::lock_guard<std::mutex> lock(g_state.mutex);
    ImGui::TextColored(g_state.tokenOk ? GREEN : MUTED, "%s", g_state.status.c_str());
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Alertes Forge : timers, auras, voix")) AlertsTab();
}

static std::map<std::string, Texture_t*> SpecTextures;
static void ReceiveTexture(const char* identifier, Texture_t* texture) {
    std::string id = identifier;
    if (id.rfind("TEX_FORGE_SPEC_", 0) == 0 && texture) SpecTextures[id.substr(15)] = texture;
}
static Texture_t* SpecTexture(const std::string& spec) {
    auto it = SpecTextures.find(spec);
    if (it != SpecTextures.end()) return it->second;
    if (API) {
        for (const auto& png : SPEC_PNGS) if (spec == png.name) { API->Textures_LoadFromMemory(("TEX_FORGE_SPEC_" + spec).c_str(), (void*)png.data, png.size, ReceiveTexture); break; }
    }
    return nullptr;
}
static void SpecIcon(const std::string& spec, float size) {
    if (Texture_t* texture = SpecTexture(spec); texture && texture->Resource) { ImGui::Image((ImTextureID)texture->Resource, ImVec2(size, size)); ImGui::SameLine(0, 4); }
}

static void AddonLoad(AddonAPI_t* api) {
    API = api;
    ImGui::SetCurrentContext((ImGuiContext*)API->ImguiContext);
    ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))API->ImguiMalloc, (void (*)(void*, void*))API->ImguiFree);
    NexusLink = (NexusLinkData_t*)API->DataLink_Get(DL_NEXUS_LINK);
    SettingsPath = std::string(API->Paths_GetAddonDirectory("Forge")) + "\\settings.json";
    CreateDirectoryA(API->Paths_GetAddonDirectory("Forge"), nullptr);
    LoadSettings(SettingsPath);
    g_addonVersion = VersionText();
    { std::lock_guard<std::mutex> lock(g_state.mutex); WindowVisible = g_state.settings.showWindow; g_fights.logsDir = g_state.settings.logsDir.empty() ? DefaultLogsDir() : Widen(g_state.settings.logsDir); }
    StartFightsWatcher();
    API->InputBinds_RegisterWithString(KB_TOGGLE, ToggleWindow, "CTRL+SHIFT+F");
    API->Textures_LoadFromMemory(TEX_ICON, (void*)FORGE_ICON_PNG, FORGE_ICON_PNG_SIZE, ReceiveTexture);
    API->Textures_LoadFromMemory(TEX_ICON_HOVER, (void*)FORGE_ICON_PNG, FORGE_ICON_PNG_SIZE, ReceiveTexture);
    API->QuickAccess_Add(QA_ICON, TEX_ICON, TEX_ICON_HOVER, KB_TOGGLE, "Forge : la soirée en direct");
    API->GUI_Register(RT_Render, RenderWindow);
    API->GUI_Register(RT_Render, RenderStrip);
    API->GUI_Register(RT_Render, RenderWidget);
    API->InputBinds_RegisterWithString(KB_WIDGET, ToggleWidget, "(null)");
    MumbleLink = (Mumble::Data*)API->DataLink_Get(DL_MUMBLE_LINK);
    API->Localization_Set(KB_TOGGLE, "fr", "Forge : ouvrir la fenêtre");
    API->Localization_Set(KB_TOGGLE, "en", "Forge: open the window");
    API->Localization_Set(KB_WIDGET, "fr", "Forge : panneau du combat");
    API->Localization_Set(KB_WIDGET, "en", "Forge: fight panel");
    AlertsLoad(API, NexusLink, MumbleLink, SettingsPath);
    API->GUI_Register(RT_Render, AlertsRender);
    // Nexus forwards arcdps' combat events to subscribers: a heartbeat that says arcdps still works.
    API->Events_Subscribe("EV_ARCDPS_COMBATEVENT_LOCAL_RAW", OnArcEventLocal);
    API->Events_Subscribe("EV_ARCDPS_COMBATEVENT_SQUAD_RAW", OnArcEventSquad);
    API->GUI_Register(RT_OptionsRender, RenderOptions);
    API->GUI_RegisterCloseOnEscape(WINDOW, &WindowVisible);
    Running = true;
    Poller = std::thread(PollLoop);
    API->Log(LOGL_INFO, "Forge", "Forge chargé.");
}

static void AddonUnload() {
    StopFightsWatcher();
    API->GUI_DeregisterCloseOnEscape(WINDOW);
    { std::lock_guard<std::mutex> lock(g_state.mutex); g_state.settings.showWindow = WindowVisible; }
    SaveSettings(SettingsPath);
    Running = false;
    if (Poller.joinable()) Poller.join();
    API->GUI_Deregister(RenderWindow);
    API->GUI_Deregister(RenderStrip);
    API->GUI_Deregister(RenderWidget);
    API->GUI_Deregister(AlertsRender);
    AlertsUnload();
    API->InputBinds_Deregister(KB_WIDGET);
    API->Events_Unsubscribe("EV_ARCDPS_COMBATEVENT_LOCAL_RAW", OnArcEventLocal);
    API->Events_Unsubscribe("EV_ARCDPS_COMBATEVENT_SQUAD_RAW", OnArcEventSquad);
    API->GUI_Deregister(RenderOptions);
    API->QuickAccess_Remove(QA_ICON);
    API->InputBinds_Deregister(KB_TOGGLE);
    API->Log(LOGL_INFO, "Forge", "Forge déchargé.");
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() {
    Def.Signature = 0x464F5247; // "FORG"
    Def.APIVersion = NEXUS_API_VERSION;
    Def.Name = "Forge";
    Def.Version = { 0, 9, 5, 0 };
    Def.Author = "Le Bus Magique";
    Def.Description = "La soirée de raid en direct : boss en cours, ta place, les mécaniques, la compo. Combats en direct, et les Alertes Forge : timers de boss (format TaimiHUD), auras d'avantages, voix.";
    Def.Load = AddonLoad;
    Def.Unload = AddonUnload;
    Def.Flags = AF_None;
    Def.Provider = UP_GitHub;
    Def.UpdateLink = "https://github.com/prxmat/forge-nexus";
    return &Def;
}
