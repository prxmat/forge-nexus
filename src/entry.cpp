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

static void ToggleWindow(const char*, bool release) {
    if (release) return;
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.settings.showWindow = !g_state.settings.showWindow;
}

static void PollLoop() {
    int tick = 0;
    while (Running) {
        if (tick % 50 == 0 || PollNow.exchange(false)) PollNight();
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

static void RenderWindow() {
    bool show;
    { std::lock_guard<std::mutex> lock(g_state.mutex); show = g_state.settings.showWindow; }
    if (!show) return;
    if (NexusLink && NexusLink->Font) ImGui::PushFont((ImFont*)NexusLink->Font);
    ImGui::SetNextWindowSize(ImVec2(520, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(WINDOW, &show, ImGuiWindowFlags_NoCollapse)) {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        ImGui::SetWindowFontScale(g_state.settings.fontScale);
        if (!g_state.hasNight) {
            ImGui::TextColored(g_state.tokenOk ? MUTED : RED, "%s", g_state.status.c_str());
            if (!g_state.tokenOk) Wrapped("Options Nexus → Forge : colle ton token Forge Uploader (Forge → Mon suivi → Réglages → Forge Uploader).");
        } else {
            const LiveNight& n = g_state.night;
            // Header: roster, day, phase.
            ImGui::TextColored(MUTED, "%s · %s", n.rosterName.c_str(), n.day.c_str());
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
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.settings.showWindow != show) { g_state.settings.showWindow = show; SaveSettings(SettingsPath); }
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
    API->InputBinds_RegisterWithString(KB_TOGGLE, ToggleWindow, "CTRL+SHIFT+F");
    API->Textures_LoadFromMemory(TEX_ICON, (void*)FORGE_ICON_PNG, FORGE_ICON_PNG_SIZE, ReceiveTexture);
    API->Textures_LoadFromMemory(TEX_ICON_HOVER, (void*)FORGE_ICON_PNG, FORGE_ICON_PNG_SIZE, ReceiveTexture);
    API->QuickAccess_Add(QA_ICON, TEX_ICON, TEX_ICON_HOVER, KB_TOGGLE, "Forge : la soirée en direct");
    API->GUI_Register(RT_Render, RenderWindow);
    API->GUI_Register(RT_OptionsRender, RenderOptions);
    API->GUI_RegisterCloseOnEscape(WINDOW, nullptr);
    Running = true;
    Poller = std::thread(PollLoop);
    API->Log(LOGL_INFO, "Forge", "Forge chargé.");
}

static void AddonUnload() {
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
    Def.Version = { 0, 1, 0, 0 };
    Def.Author = "Le Bus Magique";
    Def.Description = "La soirée de raid en direct : boss en cours, ta place, les mécaniques, la compo. Les leads mènent la soirée depuis le jeu.";
    Def.Load = AddonLoad;
    Def.Unload = AddonUnload;
    Def.Flags = AF_None;
    Def.Provider = UP_GitHub;
    Def.UpdateLink = "https://github.com/prxmat/forge-nexus";
    return &Def;
}
