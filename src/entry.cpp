// Forge, addon Nexus : la soirée de raid en direct dans le jeu, pour les joueurs et les leads.
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <cstdio>

#include "Nexus.h"
#include "imgui/imgui.h"
#include "forge.h"
#include "icon.h"

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

// Called with g_state.mutex held.
static void RenderStats() {
    if (!g_state.hasPve && !g_state.hasWvw) { ImGui::TextColored(MUTED, "%s", "Rien encore ce soir : les chiffres arrivent quelques secondes après chaque log envoyé par Forge Uploader."); return; }
    if (g_state.hasPve) {
        const PveStats& p = g_state.pve;
        ImGui::TextColored(GOLD, "%s", "Dernier essai");
        ImGui::SameLine();
        ImGui::TextColored(MUTED, "· ce soir %d essai(s), %d kill(s), %d mort(s)", p.nightPlayed, p.nightKills, p.nightDeaths);
        ImGui::TextUnformatted(p.boss.c_str());
        ImGui::SameLine();
        if (p.success) ImGui::TextColored(GREEN, "kill en %s", Clock(p.durationMs).c_str());
        else ImGui::TextColored(RED, "wipe à %.1f %% (%s)", p.hpLeft, Clock(p.durationMs).c_str());
        if (p.dps >= 0) { ImGui::Text("DPS cible : %s", Thousands(p.dps).c_str()); if (p.rank) { ImGui::SameLine(); ImGui::TextColored(MUTED, "· %de sur %d", p.rank, p.squad); } }
        ImGui::Text("Morts : %d · à terre : %d", p.deaths, p.downs);
        if (!p.fellFirst.empty()) ImGui::TextColored(RED, "Tombé(e) en premier : %s", p.fellFirst.c_str());
        for (const auto& m : p.mechanics) { ImGui::Bullet(); ImGui::SameLine(); ImGui::Text("%s ×%d", m.first.c_str(), m.second); }
        ImGui::Spacing();
    }
    if (g_state.hasWvw) {
        const WvwStats& w = g_state.wvw;
        if (g_state.hasPve) ImGui::Separator();
        ImGui::TextColored(GOLD, "%s", "McM");
        ImGui::SameLine();
        ImGui::TextColored(MUTED, "· %s : %d combat(s), %d kills / %d morts", w.title.c_str(), w.fights, w.kills, w.deaths);
        if (w.hasLast) {
            ImGui::Text("Dernier combat : %s, %s, %d en escouade%s", w.lastMap.c_str(), Clock(w.lastDuration * 1000).c_str(), w.lastSquad, w.lastAllies ? (" +" + std::to_string(w.lastAllies) + " alliés").c_str() : "");
            if (!w.teams.empty()) { std::string teams; for (const auto& t : w.teams) teams += (teams.empty() ? "" : " · ") + t.first + " " + std::to_string(t.second); ImGui::TextColored(MUTED, "Ennemis : %s", teams.c_str()); }
            ImGui::TextColored(GREEN, "%d kills, %d à terre", w.lastKills, w.lastEnemyDowns);
            ImGui::SameLine();
            ImGui::TextColored(RED, "· %d morts, %d à terre", w.lastDeaths, w.lastSquadDowns);
            ImGui::Text("Dégâts : %s%s", Thousands(w.lastDamage).c_str(), w.lastEnemyDamage >= 0 ? (" · reçus " + Thousands(w.lastEnemyDamage)).c_str() : "");
            if (w.lastStability >= 0) ImGui::Text("Stabilité : %d %% du combat", w.lastStability);
            for (const auto& f : w.findings) ImGui::TextColored(f.first == "bad" ? RED : f.first == "warn" ? GOLD : GREEN, "%s", f.second.c_str());
        }
        if (w.hasMe) {
            ImGui::Spacing();
            ImGui::TextColored(SKY, "Ma soirée (%s)", w.meRole.c_str());
            ImGui::Text("Dégâts %s (%s /s) · %d kills · %d à terre · %d morts", Thousands(w.meDamage).c_str(), Thousands(w.meDps).c_str(), w.meKills, w.meDowns, w.meDeaths);
            if (w.meDist >= 0) ImGui::Text("Distance au tag : %.0f", w.meDist);
            if (w.meStability >= 0) ImGui::Text("Stabilité générée : %.1f stacks", w.meStability);
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
            else { ImGui::Separator(); RenderStats(); }
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
                if (ImGui::BeginTabItem("Stats")) { RenderStats(); ImGui::EndTabItem(); }
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

static void RenderOptions() {
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (!TokenBuffer[0] && !g_state.settings.token.empty()) snprintf(TokenBuffer, sizeof(TokenBuffer), "%s", g_state.settings.token.c_str());
        if (!UrlBuffer[0]) snprintf(UrlBuffer, sizeof(UrlBuffer), "%s", g_state.settings.forgeUrl.c_str());
    }
    ImGui::TextWrapped("%s", "Token Forge Uploader : dans Forge, Mon suivi → Réglages → Forge Uploader → Créer mon token. Le même que pour l'uploader.");
    ImGui::InputText("Token Forge", TokenBuffer, sizeof(TokenBuffer), ImGuiInputTextFlags_Password);
    ImGui::InputText("Adresse de Forge", UrlBuffer, sizeof(UrlBuffer));
    float scale;
    { std::lock_guard<std::mutex> lock(g_state.mutex); scale = g_state.settings.fontScale; }
    if (ImGui::SliderFloat("Taille du texte", &scale, 0.8f, 1.6f, "%.1f")) { std::lock_guard<std::mutex> lock(g_state.mutex); g_state.settings.fontScale = scale; }
    if (ImGui::Button("Enregistrer et vérifier")) {
        {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.settings.token = TokenBuffer;
            g_state.settings.forgeUrl = UrlBuffer;
            while (!g_state.settings.forgeUrl.empty() && g_state.settings.forgeUrl.back() == '/') g_state.settings.forgeUrl.pop_back();
        }
        SaveSettings(SettingsPath);
        PollNow = true;
    }
    std::lock_guard<std::mutex> lock(g_state.mutex);
    ImGui::TextColored(g_state.tokenOk ? GREEN : MUTED, "%s", g_state.status.c_str());
}

static void ReceiveTexture(const char*, Texture_t*) {}

static void AddonLoad(AddonAPI_t* api) {
    API = api;
    ImGui::SetCurrentContext((ImGuiContext*)API->ImguiContext);
    ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))API->ImguiMalloc, (void (*)(void*, void*))API->ImguiFree);
    NexusLink = (NexusLinkData_t*)API->DataLink_Get(DL_NEXUS_LINK);
    SettingsPath = std::string(API->Paths_GetAddonDirectory("Forge")) + "\\settings.json";
    CreateDirectoryA(API->Paths_GetAddonDirectory("Forge"), nullptr);
    LoadSettings(SettingsPath);
    { std::lock_guard<std::mutex> lock(g_state.mutex); WindowVisible = g_state.settings.showWindow; }
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
    Def.Version = { 0, 3, 1, 0 };
    Def.Author = "Le Bus Magique";
    Def.Description = "La soirée de raid en direct : boss en cours, ta place, les mécaniques, la compo. Les leads mènent la soirée depuis le jeu.";
    Def.Load = AddonLoad;
    Def.Unload = AddonUnload;
    Def.Flags = AF_None;
    Def.Provider = UP_GitHub;
    Def.UpdateLink = "https://github.com/prxmat/forge-nexus";
    return &Def;
}
