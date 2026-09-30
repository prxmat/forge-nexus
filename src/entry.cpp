// Forge, addon Nexus : la soirée de raid en direct dans le jeu, pour les joueurs et les leads.
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <cstdio>
#include <algorithm>

#include "Nexus.h"
#include "imgui/imgui.h"
#include "forge.h"
#include "icon.h"
#include "fights.h"
#include "spec_icons.h"
#include <map>
#include <vector>

static AddonAPI_t* API = nullptr;
static AddonDefinition_t Def{};
static NexusLinkData_t* NexusLink = nullptr;
static std::string SettingsPath;
static std::atomic<bool> Running{ false };
static std::thread Poller;
static std::atomic<bool> PollNow{ false };

static const char* KB_TOGGLE = "KB_FORGE_TOGGLE";
static const char* TEX_ICON = "TEX_FORGE_ICON";
static const char* TEX_ICON_HOVER = "TEX_FORGE_ICON_HOVER";
static const char* QA_ICON = "QA_FORGE";
static const char* WINDOW = "Forge";
static void SpecIcon(const std::string& spec, float size = 18.0f);
static ImVec4 TeamColour(const std::string& name);
static const char* TeamLabel(const std::string& name);
// Nexus flips this on Escape (GUI_RegisterCloseOnEscape keeps the pointer): the window's own visibility flag.
static bool WindowVisible = true;

static void ToggleWindow(const char*, bool release) {
    if (release) return;
    WindowVisible = !WindowVisible;
}

static void PollLoop() {
    int tick = 0;
    while (Running) {
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
static void RenderStats() {
    if (!g_state.hasPve && !g_state.hasWvw) { Callout(MUTED, "Rien encore ce soir : les chiffres de Forge arrivent 20 à 40 s après chaque log envoyé par Forge Uploader. L'onglet Combats, lui, lit les logs tout de suite."); return; }
    if (g_state.hasPve) {
        const PveStats& p = g_state.pve;
        SectionTitle("Dernier essai · Forge", p.boss);
        ImGui::SameLine();
        if (p.success) ImGui::TextColored(GREEN, "  kill en %s", Clock(p.durationMs).c_str());
        else ImGui::TextColored(RED, "  wipe à %.1f %% · %s", p.hpLeft, Clock(p.durationMs).c_str());
        ImGui::Spacing();
        std::vector<Kpi> tiles;
        tiles.push_back({ p.dps >= 0 ? Thousands(p.dps) : "—", "DPS cible", p.rank ? std::to_string(p.rank) + "e sur " + std::to_string(p.squad) : "", SKY });
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

// Called with g_fights.mutex held: the fights read in game, newest first.
static void RenderCombats() {
    if (g_fights.fights.empty()) { ImGui::TextColored(MUTED, "%s", g_fights.status.c_str()); Wrapped("Les combats apparaissent ici dès qu'arcdps a écrit leur log, sans attendre l'envoi."); return; }
    // History strip.
    for (size_t index = 0; index < g_fights.fights.size() && index < 10; index++) {
        const ParsedFight& f = g_fights.fights[index];
        if (index) ImGui::SameLine();
        std::string label = f.wvw ? "McM " + Clock((int)f.durationMs) : (f.boss.empty() ? "PvE" : f.boss) + (f.success ? " ✓" : "");
        ImGui::PushStyleColor(ImGuiCol_Text, (int)index == g_fights.selected ? GOLD : MUTED);
        if (ImGui::SmallButton((label + "##" + std::to_string(index)).c_str())) g_fights.selected = (int)index;
        ImGui::PopStyleColor();
    }
    if (g_fights.selected >= (int)g_fights.fights.size()) g_fights.selected = 0;
    const ParsedFight& f = g_fights.fights[g_fights.selected];
    ImGui::Separator();
    float width = ImGui::GetContentRegionAvail().x;
    if (f.wvw) {
        ImGui::Checkbox("Escouade seulement", &g_fights.squadOnly);
        ImGui::SameLine();
        ImGui::TextColored(MUTED, "· %s · %s", Clock((int)f.durationMs).c_str(), f.file.c_str());
        uint64_t maxDamage = 1;
        int maxPlayers = 1;
        for (const auto& team : f.teams) { maxDamage = std::max(maxDamage, team.damage); maxPlayers = std::max(maxPlayers, team.players); }
        for (const auto& team : f.teams) {
            ImVec4 colour = TeamColour(team.name);
            if (NexusLink && NexusLink->FontBig) ImGui::PushFont((ImFont*)NexusLink->FontBig);
            ImGui::TextColored(colour, "%s", TeamLabel(team.name));
            if (NexusLink && NexusLink->FontBig) ImGui::PopFont();
            ImGui::SameLine();
            if (team.pov && g_fights.squadOnly) {
                ImGui::TextColored(MUTED, "escouade : %d joueurs", (int)f.squad.size());
            } else ImGui::TextColored(MUTED, "%d joueurs%s", team.players, team.pov ? " (nous)" : "");
            ImGui::Text("%d kills", team.kills); ImGui::SameLine(); ImGui::TextColored(RED, "· %d morts, %d à terre", team.deaths, team.downs); ImGui::SameLine(); ImGui::TextColored(MUTED, "· %s dégâts", Thousands((double)team.damage).c_str());
            Bar((float)team.damage / (float)maxDamage, colour, width * 0.6f);
            // Specializations as icons with counts, most played first.
            if (team.pov && g_fights.squadOnly) {
                std::map<std::string, int> counts;
                for (const auto& line : f.squad) counts[line.spec]++;
                std::vector<SpecCount> specs;
                for (const auto& [spec, count] : counts) specs.push_back({ spec, count });
                std::sort(specs.begin(), specs.end(), [](const SpecCount& a, const SpecCount& b) { return a.count > b.count; });
                int shown = 0;
                for (const auto& sc : specs) { if (shown++) ImGui::SameLine(0, 10); SpecIcon(sc.spec); ImGui::Text("%d", sc.count); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sc.spec.c_str()); }
            } else {
                int shown = 0;
                for (const auto& sc : team.specs) { if (shown >= 12) break; if (shown++) ImGui::SameLine(0, 10); SpecIcon(sc.spec); ImGui::Text("%d", sc.count); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sc.spec.c_str()); }
            }
            ImGui::Spacing();
        }
        if (f.teams.empty()) ImGui::TextColored(MUTED, "%s", "Aucune équipe reconnue dans ce log (arcdps trop ancien ?).");
    } else {
        if (NexusLink && NexusLink->FontBig) ImGui::PushFont((ImFont*)NexusLink->FontBig);
        ImGui::TextUnformatted(f.boss.c_str());
        if (NexusLink && NexusLink->FontBig) ImGui::PopFont();
        ImGui::SameLine();
        if (f.success) ImGui::TextColored(GREEN, "kill en %s", Clock((int)f.durationMs).c_str());
        else if (f.hpLeft >= 0) ImGui::TextColored(RED, "wipe à %.1f %% (%s)", f.hpLeft, Clock((int)f.durationMs).c_str());
        else ImGui::TextColored(RED, "wipe (%s)", Clock((int)f.durationMs).c_str());
        if (f.hasMe) {
            int rank = 0;
            for (size_t i = 0; i < f.squad.size(); i++) if (f.squad[i].pov) rank = (int)i + 1;
            double seconds = std::max(1.0, f.durationMs / 1000.0);
            ImGui::TextColored(SKY, "Toi : %s DPS", Thousands(f.me.damage / seconds).c_str());
            if (rank) { ImGui::SameLine(); ImGui::TextColored(MUTED, "· %de sur %d", rank, (int)f.squad.size()); }
            ImGui::SameLine();
            ImGui::TextColored(MUTED, "· %d mort(s), %d à terre", f.me.deaths, f.me.downs);
        }
        ImGui::Spacing();
    }
    // Squad table, both modes: damage bars per player.
    if (!f.squad.empty()) {
        ImGui::TextColored(GOLD, "%s", f.wvw ? "Escouade" : "Escouade (dégâts sur le boss)");
        uint64_t top = std::max<uint64_t>(1, f.squad.front().damage);
        double seconds = std::max(1.0, f.durationMs / 1000.0);
        int shown = 0;
        for (const auto& line : f.squad) {
            if (shown++ >= 12) { ImGui::TextColored(MUTED, "… et %d autres", (int)f.squad.size() - 12); break; }
            SpecIcon(line.spec, 16.0f);
            ImGui::TextColored(line.pov ? SKY : MUTED, "%-18.18s", line.name.c_str());
            ImGui::SameLine(width * 0.42f);
            Bar((float)line.damage / (float)top, line.pov ? SKY : GOLD, width * 0.28f);
            ImGui::SameLine();
            ImGui::Text("%s /s", Thousands(line.damage / seconds).c_str());
            if (line.deaths || line.downs) { ImGui::SameLine(); ImGui::TextColored(RED, "· %d↓ %d✝", line.downs, line.deaths); }
        }
    }
}

static void RenderWindow() {
    bool show = WindowVisible;
    if (!show) return;
    if (NexusLink && NexusLink->Font) ImGui::PushFont((ImFont*)NexusLink->Font);
    ImGui::SetNextWindowSize(ImVec2(520, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(WINDOW, &show, ImGuiWindowFlags_NoCollapse)) {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        ImGui::SetWindowFontScale(g_state.settings.fontScale);
        if (!g_state.hasNight) {
            ImGui::TextColored(g_state.tokenOk ? MUTED : RED, "%s", g_state.status.c_str());
            if (!g_state.tokenOk) Wrapped("Options Nexus → Forge : colle ton token Forge Uploader (Forge → Mon suivi → Réglages → Forge Uploader).");
            else { ImGui::Separator(); if (ImGui::BeginTabBar("forge-tabs-nonight")) { if (ImGui::BeginTabItem("Combats")) { std::lock_guard<std::mutex> flock(g_fights.mutex); RenderCombats(); ImGui::EndTabItem(); } if (ImGui::BeginTabItem("Forge")) { RenderStats(); ImGui::EndTabItem(); } ImGui::EndTabBar(); } }
        } else {
            const LiveNight& n = g_state.night;
            // Header: the roster (a combo when the member plays in several), day, phase.
            if (n.rosters.size() > 1) {
                ImGui::SetNextItemWidth(220);
                if (ImGui::BeginCombo("##roster", n.rosterName.c_str())) {
                    for (const auto& roster : n.rosters) {
                        std::string text = roster.name + (roster.live ? "  ● en direct" : roster.tonight ? "  · ce soir" : "");
                        if (ImGui::Selectable(text.c_str(), roster.id == n.rosterId) && roster.id != n.rosterId) {
                            g_state.settings.rosterId = roster.id;
                            SaveSettings(SettingsPath);
                            PollNow = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextColored(MUTED, "%s", n.day.c_str());
            } else ImGui::TextColored(MUTED, "%s · %s", n.rosterName.c_str(), n.day.c_str());
            ImGui::SameLine();
            if (n.phase == "live") ImGui::TextColored(RED, "%s", "● EN DIRECT");
            else if (n.phase == "ended") ImGui::TextColored(GREEN, "%s", "Soirée terminée");
            else ImGui::TextColored(MUTED, "%s", "En attente du lancement");
            // Boss strip; leads jump to a boss by clicking it.
            for (size_t index = 0; index < n.bosses.size(); index++) {
                const auto& boss = n.bosses[index];
                if (index) ImGui::SameLine();
                ImVec4 color = boss.killed ? GREEN : boss.on ? RED : MUTED;
                std::string text = std::string(boss.killed ? "✓ " : boss.on ? "▶ " : "") + boss.label;
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
            } else if (!n.hasCurrent) {
                ImGui::TextColored(MUTED, "%s", "Aucun boss prévu pour cette soirée.");
            } else if (ImGui::BeginTabBar("forge-tabs")) {
                const LiveBoss& cur = n.current;
                if (ImGui::BeginTabItem("En direct")) {
                    ImGui::TextColored(MUTED, "%s", n.phase == "live" ? "Boss en cours" : "Premier boss");
                    if (NexusLink && NexusLink->FontBig) ImGui::PushFont((ImFont*)NexusLink->FontBig);
                    ImGui::TextUnformatted(cur.label.c_str());
                    if (NexusLink && NexusLink->FontBig) ImGui::PopFont();
                    ImGui::TextColored(MUTED, "%s", "Ta place");
                    ImGui::SameLine();
                    ImGui::TextColored(SKY, "%s", cur.hasPlace ? cur.myPlace.c_str() : "—");
                    ImGui::Spacing();
                    for (const auto& goal : cur.goals) {
                        ImGui::TextColored(goal.met ? GREEN : GOLD, "%s %s", goal.met ? "✓" : "◎", goal.label.c_str());
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
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (!TokenBuffer[0] && !g_state.settings.token.empty()) snprintf(TokenBuffer, sizeof(TokenBuffer), "%s", g_state.settings.token.c_str());
        if (!UrlBuffer[0]) snprintf(UrlBuffer, sizeof(UrlBuffer), "%s", g_state.settings.forgeUrl.c_str());
    }
    ImGui::TextWrapped("%s", "Token Forge Uploader : dans Forge, Mon suivi → Réglages → Forge Uploader → Créer mon token. Le même que pour l'uploader.");
    ImGui::InputText("Token Forge", TokenBuffer, sizeof(TokenBuffer), ImGuiInputTextFlags_Password);
    ImGui::InputText("Adresse de Forge", UrlBuffer, sizeof(UrlBuffer));
    { std::lock_guard<std::mutex> flock(g_fights.mutex); if (!LogsBuffer[0]) snprintf(LogsBuffer, sizeof(LogsBuffer), "%s", Narrow(g_fights.logsDir).c_str()); }
    ImGui::InputText("Dossier des logs arcdps", LogsBuffer, sizeof(LogsBuffer));
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
    std::lock_guard<std::mutex> lock(g_state.mutex);
    ImGui::TextColored(g_state.tokenOk ? GREEN : MUTED, "%s", g_state.status.c_str());
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
    { std::lock_guard<std::mutex> lock(g_state.mutex); WindowVisible = g_state.settings.showWindow; g_fights.logsDir = g_state.settings.logsDir.empty() ? DefaultLogsDir() : Widen(g_state.settings.logsDir); }
    StartFightsWatcher();
    API->InputBinds_RegisterWithString(KB_TOGGLE, ToggleWindow, "CTRL+SHIFT+F");
    API->Textures_LoadFromMemory(TEX_ICON, (void*)FORGE_ICON_PNG, FORGE_ICON_PNG_SIZE, ReceiveTexture);
    API->Textures_LoadFromMemory(TEX_ICON_HOVER, (void*)FORGE_ICON_PNG, FORGE_ICON_PNG_SIZE, ReceiveTexture);
    API->QuickAccess_Add(QA_ICON, TEX_ICON, TEX_ICON_HOVER, KB_TOGGLE, "Forge : la soirée en direct");
    API->GUI_Register(RT_Render, RenderWindow);
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
    Def.Version = { 0, 4, 2, 0 };
    Def.Author = "Le Bus Magique";
    Def.Description = "La soirée de raid en direct : boss en cours, ta place, les mécaniques, la compo. Les leads mènent la soirée depuis le jeu.";
    Def.Load = AddonLoad;
    Def.Unload = AddonUnload;
    Def.Flags = AF_None;
    Def.Provider = UP_GitHub;
    Def.UpdateLink = "https://github.com/prxmat/forge-nexus";
    return &Def;
}
