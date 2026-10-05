#include "alerts.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "imgui/imgui.h"
#include "auras.h"
#include "boons.h"
#include "forge.h"
#include "speech.h"
#include "timers.h"
#include "tones.h"
#include "thirdparty/miniz/miniz.h"

namespace fs = std::filesystem;

namespace {
AddonAPI_t* Api = nullptr;
NexusLinkData_t* Nexus = nullptr;
Mumble::Data* Link = nullptr;
std::string SettingsFile;

const ImVec4 RED{ 0.957f, 0.263f, 0.212f, 1.0f };
const ImVec4 GREEN{ 0.298f, 0.788f, 0.549f, 1.0f };
const ImVec4 MUTED{ 0.651f, 0.678f, 0.733f, 1.0f };
const ImVec4 GOLD{ 1.0f, 0.812f, 0.302f, 1.0f };

constexpr int KEY_COUNT = 5;
const char* KEY_IDS[KEY_COUNT] = { "KB_FORGE_TIMER_KEY_0", "KB_FORGE_TIMER_KEY_1", "KB_FORGE_TIMER_KEY_2", "KB_FORGE_TIMER_KEY_3", "KB_FORGE_TIMER_KEY_4" };
const char* KB_RESET = "KB_FORGE_TIMERS_RESET";
const char* HERO_URL = "https://github.com/QuitarHero/Hero-Timers/releases/latest/download/Hero.Timer.Pack.zip";

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string Clock(double seconds) { int s = (int)std::max(0.0, seconds); char text[16]; snprintf(text, sizeof(text), "%d:%02d", s / 60, s % 60); return text; }
ImU32 WithAlpha(ImU32 colour, float alpha) { return (colour & ~IM_COL32_A_MASK) | ((ImU32)(std::max(0.0f, std::min(1.0f, alpha)) * 255.0f) << IM_COL32_A_SHIFT); }
ImU32 Rgb(const timers::Colour& colour, ImU32 fallback, float alpha = 1.0f) { return colour.set ? IM_COL32(colour.r, colour.g, colour.b, (int)(alpha * 255)) : WithAlpha(fallback, alpha); }
std::wstring Wide(const std::string& text) {
    if (text.empty()) return L"";
    int size = MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), (int)text.size(), &out[0], size);
    return out;
}

// ---- The timer library: Forge's own folder first, then TaimiHUD's ----------------------------------------------

struct Library {
    std::vector<std::shared_ptr<const timers::TimerFile>> files;
    std::vector<std::string> errors;
    int fromForge = 0, fromTaimi = 0;
    bool loaded = false;
};
std::mutex LibraryMutex;
std::shared_ptr<const Library> Lib = std::make_shared<Library>();
std::atomic<int> LibraryVersion{ 0 };
std::atomic<bool> Loading{ false }, ReloadRequested{ false };
std::thread Loader;
std::string ForgeDir, SoundsDir;
std::vector<std::string> TaimiDirs;

std::shared_ptr<const Library> CurrentLibrary() { std::lock_guard<std::mutex> lock(LibraryMutex); return Lib; }

// Render thread only (it owns Loader).
void ReadTimers(bool withTaimi) {
    if (Loading.exchange(true)) return;
    if (Loader.joinable()) Loader.join();
    std::string forge = ForgeDir;
    std::vector<std::string> taimi = withTaimi ? TaimiDirs : std::vector<std::string>();
    Loader = std::thread([forge, taimi]() {
        auto lib = std::make_shared<Library>();
        std::vector<std::shared_ptr<const timers::TimerFile>> own, theirs;
        timers::LoadFolder(forge, "Forge", own, lib->errors);
        for (const auto& folder : taimi) timers::LoadFolder(folder, "TaimiHUD", theirs, lib->errors);
        std::set<std::string> ids;
        for (const auto& file : own) if (ids.insert(file->id).second) { lib->files.push_back(file); lib->fromForge++; }
        for (const auto& file : theirs) if (ids.insert(file->id).second) { lib->files.push_back(file); lib->fromTaimi++; }
        // Hero's file names carry the game's order (04-R_W2B1…): keep it.
        std::sort(lib->files.begin(), lib->files.end(), [](const auto& a, const auto& b) { return fs::path(a->path).filename() < fs::path(b->path).filename(); });
        lib->loaded = true;
        { std::lock_guard<std::mutex> lock(LibraryMutex); Lib = lib; }
        LibraryVersion++;
        Loading = false;
    });
}

// ---- Hero's Timers, downloaded into Forge's folder --------------------------------------------------------------

std::atomic<bool> Downloading{ false };
std::thread Downloader;
std::mutex DownloadMutex;
std::string DownloadStatus;

void SetDownloadStatus(const std::string& text) { std::lock_guard<std::mutex> lock(DownloadMutex); DownloadStatus = text; }
std::string GetDownloadStatus() { std::lock_guard<std::mutex> lock(DownloadMutex); return DownloadStatus; }

// A zip entry may only land inside the target folder: no root, no drive, no "..".
bool SafeEntry(const std::string& name) {
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string::npos) return false;
    std::string path = name;
    std::replace(path.begin(), path.end(), '\\', '/');
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        if (end - start == 2 && path.compare(start, 2, "..") == 0) return false;
        start = end + 1;
    }
    return true;
}

// Downloads a zip and unpacks it into root, which is ours and is emptied first (only when its name is the one
// expected, a safeguard). Returns "" or what went wrong; found counts the files with one of the extensions.
std::string UnpackZip(const std::string& url, const fs::path& root, const char* expectedName, const std::vector<std::string>& extensions, int& found) {
    found = 0;
    std::string body;
    int code = HttpGet(url, "", body);
    if (code != 200 || body.size() < 1024) return "Téléchargement impossible (HTTP " + std::to_string(code) + ").";
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, body.data(), body.size(), 0)) return "Archive illisible.";
    std::error_code error;
    if (root.filename() == fs::path(expectedName)) fs::remove_all(root, error);
    fs::create_directories(root, error);
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint index = 0; index < count; index++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_is_directory) continue;
        std::string name = stat.m_filename;
        if (!SafeEntry(name)) continue;
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, index, &size, 0);
        if (!data) continue;
        fs::path out = root / fs::u8path(name);
        fs::create_directories(out.parent_path(), error);
        std::ofstream file(out, std::ios::binary);
        file.write((const char*)data, (std::streamsize)size);
        mz_free(data);
        std::string extension = out.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (file && std::find(extensions.begin(), extensions.end(), extension) != extensions.end()) found++;
    }
    mz_zip_reader_end(&zip);
    return "";
}

// Render thread only (it owns Downloader).
void DownloadHero() {
    if (Downloading.exchange(true)) return;
    if (Downloader.joinable()) Downloader.join();
    std::string target = ForgeDir.empty() ? "" : (fs::path(ForgeDir) / "Hero-Timers").string();
    Downloader = std::thread([target]() {
        auto finish = [](const std::string& status) { SetDownloadStatus(status); Downloading = false; };
        if (target.empty()) return finish("Dossier de Forge introuvable.");
        SetDownloadStatus("Téléchargement de Hero's Timers…");
        int found = 0;
        std::string failure = UnpackZip(HERO_URL, fs::path(target), "Hero-Timers", { ".bhtimer" }, found);
        if (!failure.empty()) return finish(failure);
        if (!found) return finish("Aucun timer dans l'archive.");
        ReloadRequested = true;
        finish("Hero's Timers installés : " + std::to_string(found) + " timers.");
    });
}

// ---- WeakAuras' sounds, downloaded into Forge's sounds folder ---------------------------------------------------
// Kept beside the addon, not inside it: they are WeakAuras' (CC BY 3.0, Sampling Plus 1.0, CC0, GPL-2.0, credits in
// the archive's CREDITS.txt), served from the addon's repository.

const char* WEAKAURAS_SOUNDS_URL = "https://raw.githubusercontent.com/prxmat/forge-nexus/main/sounds/weakauras.zip";
std::atomic<bool> SoundsDownloading{ false };
std::thread SoundsDownloader;
std::mutex SoundsMutex;
std::string SoundsStatus;

void SetSoundsStatus(const std::string& text) { std::lock_guard<std::mutex> lock(SoundsMutex); SoundsStatus = text; }
std::string GetSoundsStatus() { std::lock_guard<std::mutex> lock(SoundsMutex); return SoundsStatus; }

// Render thread, or the addon's load (before any frame).
void InstallWeakAurasSounds() {
    if (SoundsDownloading.exchange(true)) return;
    if (SoundsDownloader.joinable()) SoundsDownloader.join();
    fs::path root = fs::path(SoundsDir) / "WeakAuras";
    SoundsDownloader = std::thread([root]() {
        SetSoundsStatus("Téléchargement des sons WeakAuras…");
        int found = 0;
        std::string failure = UnpackZip(WEAKAURAS_SOUNDS_URL, root, "WeakAuras", { ".ogg", ".wav" }, found);
        SetSoundsStatus(!failure.empty() ? failure : found ? "Sons WeakAuras installés : " + std::to_string(found) + "." : "Aucun son dans l'archive.");
        SoundsDownloading = false;
    });
}

// ---- The machines of the current map (render thread) ------------------------------------------------------------

struct Running {
    std::vector<std::unique_ptr<timers::Machine>> machines;
    uint32_t map = 0;
    int libraryVersion = -1, settingsVersion = -1;
    uint8_t server[28]{};
    bool combat = false;
} Run;
std::atomic<int> SettingsVersion{ 0 };
std::atomic<uint32_t> KeysDown{ 0 }, KeysUp{ 0 };
std::atomic<bool> ResetRequested{ false };

// Hero's Harvest Temple CM simulation is a practice tool: it starts out of combat near its spot and keeps « Begin
// Simulation » up until trigger key 0. Off until the player turns it on.
bool OffByDefault(const timers::TimerFile& file) {
    std::string category = file.category;
    std::transform(category.begin(), category.end(), category.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return category == "simulation" || file.id.find(".simulation.") != std::string::npos;
}

bool Has(const std::vector<std::string>& list, const std::string& id) { return std::find(list.begin(), list.end(), id) != list.end(); }

bool Enabled(const Settings& st, const timers::TimerFile& file) { return OffByDefault(file) ? Has(st.timersOptIn, file.id) : !Has(st.timersOff, file.id); }

void Sync(const Settings& st) {
    uint32_t map = Link ? Link->Context.MapID : 0;
    bool moved = false;
    if (Link) {
        static const uint8_t zero[28] = {};
        if (memcmp(Link->Context.ServerAddress, zero, sizeof(zero)) != 0 && memcmp(Link->Context.ServerAddress, Run.server, sizeof(zero)) != 0) {
            memcpy(Run.server, Link->Context.ServerAddress, sizeof(zero));
            moved = true;
        }
    }
    if (map != Run.map || moved) {
        // Another map or another instance: every timer starts over, and the boons we followed are gone.
        Run.machines.clear();
        Run.map = map;
        Run.libraryVersion = -1;
        AurasClear();
    }
    int libraryVersion = LibraryVersion, settingsVersion = SettingsVersion;
    if (libraryVersion == Run.libraryVersion && settingsVersion == Run.settingsVersion) return;
    Run.libraryVersion = libraryVersion;
    Run.settingsVersion = settingsVersion;
    auto lib = CurrentLibrary();
    std::vector<std::unique_ptr<timers::Machine>> next;
    if (st.timersOn) {
        for (const auto& file : lib->files) {
            if (file->map != map || !Enabled(st, *file)) continue;
            // A machine already running this very file keeps its place in the fight.
            auto it = std::find_if(Run.machines.begin(), Run.machines.end(), [&](const auto& machine) { return machine && machine->Shared() == file; });
            if (it != Run.machines.end()) next.push_back(std::move(*it));
            else next.push_back(std::make_unique<timers::Machine>(file));
        }
    }
    Run.machines = std::move(next);
}

void TickTimers(const Settings& st, double now, std::vector<timers::Bar>& bars, std::vector<timers::Shout>& shouts) {
    Sync(st);
    bool combat = Link && Link->Context.IsInCombat;
    if (combat != Run.combat) {
        Run.combat = combat;
        for (auto& machine : Run.machines) machine->SetCombat(combat ? timers::Combat::Entered : timers::Combat::Exited);
    }
    uint32_t down = KeysDown.exchange(0), up = KeysUp.exchange(0);
    if (ResetRequested.exchange(false)) for (auto& machine : Run.machines) machine->Reset();
    for (int key = 0; key < KEY_COUNT; key++) if (down & (1u << key)) for (auto& machine : Run.machines) machine->Press(key);
    if (Link) {
        timers::Vec3 pos{ Link->AvatarPosition.X, Link->AvatarPosition.Y, Link->AvatarPosition.Z };
        for (auto& machine : Run.machines) machine->Tick(now, pos, (float)st.warnAt, bars, shouts);
    }
    for (int key = 0; key < KEY_COUNT; key++) if (up & (1u << key)) for (auto& machine : Run.machines) machine->Release(key);
}

void OnKey(const char* identifier, bool release) {
    if (!identifier) return;
    if (strcmp(identifier, KB_RESET) == 0) { if (!release) ResetRequested = true; return; }
    for (int key = 0; key < KEY_COUNT; key++) {
        if (strcmp(identifier, KEY_IDS[key]) != 0) continue;
        (release ? KeysUp : KeysDown).fetch_or(1u << key);
        return;
    }
}

// ---- Textures: the pack's icons from disk, the boons' from the game's render service ------------------------------

std::mutex TextureMutex;
std::map<std::string, Texture_t*> Textures;
std::set<std::string> Requested;

void OnTexture(const char* identifier, Texture_t* texture) {
    if (!identifier || !texture) return;
    std::lock_guard<std::mutex> lock(TextureMutex);
    Textures[identifier] = texture;
}

// Nexus may call back at once: the lock is released before asking.
Texture_t* Ask(const std::string& id, const std::function<void()>& load) {
    {
        std::lock_guard<std::mutex> lock(TextureMutex);
        auto it = Textures.find(id);
        if (it != Textures.end()) return it->second;
        if (!Requested.insert(id).second) return nullptr;
    }
    load();
    std::lock_guard<std::mutex> lock(TextureMutex);
    auto it = Textures.find(id);
    return it != Textures.end() ? it->second : nullptr;
}

Texture_t* FileTexture(const std::string& path) {
    if (path.empty() || !Api) return nullptr;
    std::string id = "TEX_FORGE_TMR_" + std::to_string(std::hash<std::string>{}(path));
    return Ask(id, [&]() { Api->Textures_LoadFromFile(id.c_str(), path.c_str(), OnTexture); });
}

Texture_t* BoonTexture(const BoonInfo* boon) {
    if (!boon || !Api) return nullptr;
    std::string id = "TEX_FORGE_BOON_" + std::to_string(boon->id);
    return Ask(id, [&]() { Api->Textures_LoadFromURL(id.c_str(), "https://render.guildwars2.com", boon->icon, OnTexture); });
}

// ---- What is on screen ------------------------------------------------------------------------------------------

struct Flash { std::string text; ImU32 colour; double until; };
std::vector<Flash> Flashes; // Timer sounds' text, in the centre.
std::vector<Flash> Toasts;  // Small notes under the bars (a timer re-armed).
double DemoStart = 0, DemoUntil = 0;

std::string TimerVoice(const Settings& st) { return st.timerVoice.empty() ? SpeechVoiceFor("en") : st.timerVoice; }
void SayTimer(const Settings& st, const std::string& text) { Speak(text, TimerVoice(st), st.voiceVolume, st.voiceRate); }
void SayAura(const Settings& st, const std::string& text) { Speak(text, st.auraVoice, st.voiceVolume, st.voiceRate); }

// A warning's sound: its own when the player chose one for that mechanic, else the general one.
std::string WarningSoundFor(const Settings& st, const timers::TimerFile* timer, const std::string& text, bool due) {
    if (timer) {
        auto it = st.warningSounds.find(timer->id + "\n" + text);
        if (it != st.warningSounds.end()) {
            const std::string& own = due ? it->second.due : it->second.countdown;
            if (own != "default") return own;
        }
    }
    return due ? st.dueSound : st.countdownSound;
}

// The countdown sounds on the last N seconds, every M: 3 and 1 → 3, 2, 1.
bool CountdownSecond(const Settings& st, int seconds) { return seconds >= 1 && seconds <= st.countdownFrom && seconds % std::max(1, st.countdownEvery) == 0; }

std::string SoundName(const std::string& id) {
    if (id == "default") return "Par défaut";
    if (id.empty()) return "Aucun";
    if (const char* label = tones::Label(id)) return label;
    if (id.rfind("file:", 0) == 0) {
        // A file: its name without folder nor extension ("WeakAuras/AirHorn.ogg" → AirHorn).
        std::string name = id.substr(5);
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        size_t dot = name.find_last_of('.');
        return dot == std::string::npos ? name : name.substr(0, dot);
    }
    return id;
}

bool FromWeakAuras(const std::string& file) { return file.rfind("WeakAuras/", 0) == 0; }

// A menu of sounds (silence, the synthesized ones, the player's .wav files); picking one plays it.
bool SoundCombo(const char* label, std::string& id, const std::vector<std::string>& files, bool withDefault, int volume) {
    bool changed = false;
    if (ImGui::BeginCombo(label, SoundName(id).c_str())) {
        auto option = [&](const std::string& value) {
            if (!ImGui::Selectable((SoundName(value) + "##" + value).c_str(), id == value)) return;
            id = value;
            changed = true;
            if (value != "default") PlayTone(value, volume);
        };
        if (withDefault) option("default");
        option("");
        ImGui::TextDisabled("%s", "Forge");
        for (const auto& sound : tones::All()) option(sound.id);
        bool header = false;
        for (const auto& file : files) {
            if (!FromWeakAuras(file)) continue;
            if (!header) { ImGui::TextDisabled("%s", "WeakAuras"); header = true; }
            option("file:" + file);
        }
        header = false;
        for (const auto& file : files) {
            if (FromWeakAuras(file)) continue;
            if (!header) { ImGui::TextDisabled("%s", "Mes sons"); header = true; }
            option("file:" + file);
        }
        ImGui::EndCombo();
    }
    return changed;
}

// A timer's warnings, each once, in the order the file has them.
std::vector<std::string> WarningsOf(const timers::TimerFile& file) {
    std::vector<std::string> out;
    for (const auto& phase : file.phases)
        for (const auto& alert : phase.alerts)
            if (alert.hasWarning && std::find(out.begin(), out.end(), alert.warning) == out.end()) out.push_back(alert.warning);
    return out;
}

void Outlined(ImDrawList* draw, ImFont* font, float size, ImVec2 pos, ImU32 colour, const char* text) {
    ImU32 shadow = WithAlpha(IM_COL32(0, 0, 0, 255), ((colour >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f * 0.9f);
    const float o = std::max(1.0f, size / 18.0f);
    const ImVec2 offsets[] = { { -o, 0 }, { o, 0 }, { 0, -o }, { 0, o }, { o, o } };
    for (const auto& offset : offsets) draw->AddText(font, size, ImVec2(pos.x + offset.x, pos.y + offset.y), shadow, text);
    draw->AddText(font, size, pos, colour, text);
}

// A window the player drags while the alerts are unlocked; locked, clicks go through it to the game.
bool BeginOverlay(const char* name, float x, float y, bool placing) {
    ImGui::SetNextWindowPos(ImVec2(x, y), placing ? ImGuiCond_Appearing : ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(placing ? 0.6f : 0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (!placing) flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;
    return ImGui::Begin(name, nullptr, flags);
}

void SavePlace(float Settings::*x, float Settings::*y) {
    if (ImGui::IsMouseDown(0)) return;
    ImVec2 at = ImGui::GetWindowPos();
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.settings.*x == at.x && g_state.settings.*y == at.y) return;
    g_state.settings.*x = at.x;
    g_state.settings.*y = at.y;
    SaveSettingsLocked(SettingsFile);
}

// An alert stays in the centre 5 s at most: one that lasts (« Begin Simulation », 9999 s) goes on as a bar.
constexpr float CENTRE_SECONDS = 5.0f;
bool InCentre(const timers::Bar& bar) { return !bar.warning && bar.duration - bar.remaining < CENTRE_SECONDS; }

// The warnings as WeakAuras bars: icon, text, a fill that drains to the moment, the seconds left. Alerts join them
// when the centre text is off, or once their time in the centre is over.
void RenderBars(const Settings& st, std::vector<timers::Bar> bars, double now) {
    bars.erase(std::remove_if(bars.begin(), bars.end(), [&](const timers::Bar& bar) { return st.centerText && InCentre(bar); }), bars.end());
    std::stable_sort(bars.begin(), bars.end(), [](const timers::Bar& a, const timers::Bar& b) { return a.remaining < b.remaining; });
    bool placing = !st.alertsLocked;
    if (bars.empty() && Toasts.empty() && !placing) return;
    const float scale = st.fontScale;
    if (BeginOverlay("Forge alertes", st.barsX, st.barsY, placing)) {
        ImGui::SetWindowFontScale(scale);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const float width = st.barWidth * scale, height = 24.0f * scale, text = ImGui::GetFontSize();
        if (placing && bars.empty()) {
            timers::Bar sample;
            sample.text = "Alertes Forge : glisse pour placer";
            sample.remaining = 7.5f; sample.duration = 10.0f;
            bars.push_back(sample);
        }
        int shown = 0;
        for (const auto& bar : bars) {
            if (shown++ >= 8) break;
            ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + width, p0.y + height);
            draw->AddRectFilled(p0, p1, IM_COL32(21, 27, 34, 215), 4.0f);
            float fraction = bar.warning && bar.duration > 0 ? std::max(0.0f, std::min(1.0f, bar.remaining / bar.duration)) : 1.0f;
            float alpha = bar.warning ? 0.85f : std::min(1.0f, bar.remaining / 0.4f) * 0.85f;
            ImU32 fill = Rgb(bar.fill, bar.warning ? IM_COL32(58, 166, 217, 255) : ImGui::GetColorU32(GOLD), alpha);
            float left = p0.x + height;
            draw->AddRectFilled(ImVec2(left, p0.y), ImVec2(left + (p1.x - left) * fraction, p1.y), fill, 4.0f, ImDrawCornerFlags_Right);
            if (Texture_t* icon = FileTexture(bar.icon); icon && icon->Resource) draw->AddImage((ImTextureID)icon->Resource, p0, ImVec2(p0.x + height, p1.y));
            else draw->AddRectFilled(p0, ImVec2(p0.x + height, p1.y), IM_COL32(40, 46, 54, 255), 4.0f, ImDrawCornerFlags_Left);
            float y = p0.y + (height - text) / 2;
            Outlined(draw, font, text, ImVec2(left + 6.0f * scale, y), Rgb(bar.colour, IM_COL32(245, 245, 245, 255)), bar.text.c_str());
            if (bar.warning) {
                char seconds[16];
                snprintf(seconds, sizeof(seconds), bar.remaining < 10 ? "%.1f" : "%.0f", bar.remaining);
                float w = font->CalcTextSizeA(text, FLT_MAX, 0, seconds).x;
                Outlined(draw, font, text, ImVec2(p1.x - w - 6.0f * scale, y), IM_COL32(245, 245, 245, 255), seconds);
                if (bar.remaining <= 3.0f) {
                    float pulse = 0.5f + 0.5f * std::sin((float)now * 9.0f);
                    draw->AddRect(p0, p1, WithAlpha(ImGui::GetColorU32(RED), 0.45f + 0.55f * pulse), 4.0f, ImDrawCornerFlags_All, 2.0f * scale);
                }
            }
            ImGui::Dummy(ImVec2(width, height));
        }
        for (const auto& toast : Toasts) ImGui::TextColored(MUTED, "%s", toast.text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        if (placing) SavePlace(&Settings::barsX, &Settings::barsY);
    }
    ImGui::End();
}

// Big text in the middle of the screen: alerts, the last seconds of warnings, the timers' spoken lines.
void RenderCentre(const Settings& st, const std::vector<timers::Bar>& bars, double now) {
    if (!st.centerText) return;
    struct Line { std::string text; ImU32 colour; };
    std::vector<Line> lines;
    auto add = [&](const std::string& text, ImU32 colour) {
        for (const auto& line : lines) if (line.text == text) return;
        if (lines.size() < 4) lines.push_back({ text, colour });
    };
    for (const auto& bar : bars) if (InCentre(bar)) add(bar.text, Rgb(bar.colour, ImGui::GetColorU32(GOLD), std::min({ 1.0f, bar.remaining / 0.4f, (CENTRE_SECONDS - (bar.duration - bar.remaining)) / 0.4f })));
    if (st.warnAt > 0) {
        for (const auto& bar : bars) {
            if (!bar.warning || bar.remaining > st.warnAt) continue;
            add(bar.text + "  " + std::to_string((int)std::ceil(bar.remaining)), Rgb(bar.colour, IM_COL32(255, 255, 255, 255)));
        }
    }
    for (const auto& flash : Flashes) add(flash.text, WithAlpha(flash.colour, std::min(1.0, (flash.until - now) / 0.4)));
    if (lines.empty()) return;
    ImFont* font = Nexus && Nexus->FontBig ? (ImFont*)Nexus->FontBig : ImGui::GetFont();
    float size = font->FontSize * st.centerScale;
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    float y = screen.y * st.centerY;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    for (const auto& line : lines) {
        ImVec2 dims = font->CalcTextSizeA(size, FLT_MAX, 0, line.text.c_str());
        Outlined(draw, font, size, ImVec2((screen.x - dims.x) / 2, y), line.colour, line.text.c_str());
        y += dims.y * 1.08f;
    }
}

// A WeakAuras-style swipe: the share of the boon already gone, darkened clockwise from the top.
void Sweep(ImDrawList* draw, ImVec2 p0, ImVec2 p1, float gone, ImU32 colour) {
    if (gone <= 0.01f) return;
    gone = std::min(gone, 1.0f);
    ImVec2 c((p0.x + p1.x) / 2, (p0.y + p1.y) / 2);
    float r = (p1.x - p0.x) * 0.75f;
    const float pi = 3.14159265f;
    float start = -pi / 2, end = start + gone * 2 * pi;
    int steps = std::max(2, (int)(gone * 48));
    draw->PushClipRect(p0, p1, true);
    for (int i = 0; i < steps; i++) {
        float a = start + (end - start) * i / steps, b = start + (end - start) * (i + 1) / steps;
        draw->AddTriangleFilled(c, ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), ImVec2(c.x + std::cos(b) * r, c.y + std::sin(b) * r), colour);
    }
    draw->PopClipRect();
}

std::map<uint32_t, BoonState> DemoBoons(double now) {
    double t = now - DemoStart;
    std::map<uint32_t, BoonState> out;
    auto set = [&](uint32_t id, int stacks, double left, double peak) { BoonState s; s.stacks = stacks; s.remainingMs = (uint32_t)(std::max(0.0, left) * 1000); s.peakMs = (uint32_t)(peak * 1000); out[id] = s; };
    set(1187, 1, 10 - std::fmod(t, 10), 10);
    set(740, 14, 18 - std::fmod(t, 18), 18);
    set(1122, t < 6 ? 3 : 0, 6 - t, 6);
    set(725, 1, 25 - t, 25);
    return out;
}

std::map<uint32_t, double> MissingSince, LastAnnounce;
std::set<uint32_t> Announced;

// The boons as icons: time left as a swipe and in seconds, stacks in the corner, a pulsing red frame when one is
// missing in combat (and its sound, once it has been missing 1.5 s).
void RenderAuras(const Settings& st, double now, bool demo) {
    if (!st.aurasOn) return;
    bool placing = !st.alertsLocked;
    bool combat = Link && Link->Context.IsInCombat;
    std::map<uint32_t, BoonState> states = demo ? DemoBoons(now) : AurasSnapshot();
    bool tracking = demo || AurasEverBuff();
    struct View { const BoonInfo* boon; BoonState state; bool enough, alarm; };
    std::vector<View> views;
    for (const auto& rule : st.auras) {
        const BoonInfo* boon = FindBoon(rule.buff);
        if (!rule.on || !boon) continue;
        View view{ boon, states.count(rule.buff) ? states[rule.buff] : BoonState{}, false, false };
        bool present = view.state.remainingMs > 0;
        view.enough = boon->intensity ? view.state.stacks >= std::max(1, rule.minStacks) : present;
        view.alarm = tracking && (!rule.combatOnly || combat || demo) && !view.enough;
        if (view.alarm) {
            if (!MissingSince.count(rule.buff)) MissingSince[rule.buff] = now;
            if (!Announced.count(rule.buff) && now - MissingSince[rule.buff] >= 1.5 && now - LastAnnounce[rule.buff] >= 6.0) {
                Announced.insert(rule.buff);
                LastAnnounce[rule.buff] = now;
                if (rule.sound == 1) SayAura(st, boon->name);
                else if (rule.sound == 2) PlayAlertSound(1);
            }
        } else {
            MissingSince.erase(rule.buff);
            Announced.erase(rule.buff);
        }
        bool visible = rule.show == 0 || (rule.show == 1 && present) || (rule.show == 2 && view.alarm);
        if (visible || placing) views.push_back(view);
    }
    if (views.empty()) return;
    if (BeginOverlay("Forge auras", st.aurasX, st.aurasY, placing)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const float size = st.auraSize, gap = 6.0f, small = std::max(12.0f, size * 0.34f);
        ImVec2 origin = ImGui::GetCursorScreenPos();
        for (size_t index = 0; index < views.size(); index++) {
            const View& view = views[index];
            ImVec2 p0(origin.x + index * (size + gap), origin.y), p1(p0.x + size, p0.y + size);
            bool present = view.state.remainingMs > 0;
            ImU32 tint = view.enough ? IM_COL32_WHITE : IM_COL32(120, 120, 120, present ? 230 : 150);
            if (Texture_t* icon = BoonTexture(view.boon); icon && icon->Resource) draw->AddImageRounded((ImTextureID)icon->Resource, p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint, 4.0f);
            else {
                draw->AddRectFilled(p0, p1, IM_COL32(40, 46, 54, 230), 4.0f);
                std::string initial(view.boon->name, view.boon->name[0] & 0x80 ? 2 : 1);
                Outlined(draw, font, small * 1.3f, ImVec2(p0.x + size * 0.3f, p0.y + size * 0.25f), IM_COL32(230, 230, 230, 255), initial.c_str());
            }
            if (present && view.state.peakMs) Sweep(draw, p0, p1, 1.0f - (float)view.state.remainingMs / (float)view.state.peakMs, IM_COL32(0, 0, 0, 120));
            if (view.alarm) {
                float pulse = 0.5f + 0.5f * std::sin((float)now * 7.0f);
                draw->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), WithAlpha(ImGui::GetColorU32(RED), 0.5f + 0.5f * pulse), 5.0f, ImDrawCornerFlags_All, 3.0f);
            } else draw->AddRect(p0, p1, IM_COL32(0, 0, 0, 200), 4.0f, ImDrawCornerFlags_All, 1.0f);
            if (present) {
                double left = view.state.remainingMs / 1000.0;
                char text[16];
                if (left >= 60) snprintf(text, sizeof(text), "%d:%02d", (int)left / 60, (int)left % 60);
                else snprintf(text, sizeof(text), left < 3 ? "%.1f" : "%.0f", left);
                ImVec2 dims = font->CalcTextSizeA(small, FLT_MAX, 0, text);
                Outlined(draw, font, small, ImVec2(p0.x + (size - dims.x) / 2, p1.y - dims.y - 1), left < 3 ? ImGui::GetColorU32(GOLD) : IM_COL32(255, 255, 255, 255), text);
            }
            if (view.boon->intensity && view.state.stacks > 0) {
                std::string stacks = std::to_string(view.state.stacks);
                ImVec2 dims = font->CalcTextSizeA(small, FLT_MAX, 0, stacks.c_str());
                Outlined(draw, font, small, ImVec2(p1.x - dims.x - 2, p0.y + 1), view.enough ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(RED), stacks.c_str());
            }
        }
        ImGui::Dummy(ImVec2(views.size() * (size + gap) - gap, size));
        if (placing) {
            ImGui::TextColored(MUTED, "%s", "Auras : glisse pour placer");
            SavePlace(&Settings::aurasX, &Settings::aurasY);
        }
    }
    ImGui::End();
}

void DemoBars(double now, std::vector<timers::Bar>& bars) {
    double t = now - DemoStart;
    timers::Bar next;
    next.text = "Prochain champignon";
    next.remaining = (float)std::max(0.0, 12.0 - t); next.duration = 12.0f; next.fill = { 24, 173, 74, true };
    bars.push_back(next);
    timers::Bar move;
    move.text = "Bouge !";
    move.remaining = (float)(4.0 - std::fmod(t, 4.0)); move.duration = 4.0f; move.fill = { 202, 120, 60, true };
    bars.push_back(move);
    if (t > 9.5) {
        timers::Bar spawned;
        spawned.text = "Champignon apparu !";
        spawned.remaining = (float)std::max(0.0, 12.0 - t); spawned.duration = 2.5f; spawned.warning = false; spawned.colour = { 255, 207, 77, true };
        bars.push_back(spawned);
    }
}

// The demo's « Bouge ! » every 4 s: its countdown seconds and its moment, as a real warning would sound them.
void DemoShouts(double before, double now, std::vector<timers::Shout>& shouts) {
    for (int round = 1; round <= 3; round++) {
        for (int left = 0; left <= 3; left++) {
            double at = DemoStart + 4.0 * round - left;
            if (!(before < at && at <= now)) continue;
            timers::Shout shout;
            shout.kind = left ? timers::Shout::Tick : timers::Shout::Due;
            shout.text = "Bouge !";
            shout.seconds = left;
            shouts.push_back(shout);
        }
    }
    double spawn = DemoStart + 9.5;
    if (before < spawn && spawn <= now) {
        timers::Shout shout;
        shout.kind = timers::Shout::Alert;
        shout.text = "Champignon apparu !";
        shouts.push_back(shout);
    }
}

// What a machine is waiting for, in words.
std::string Waiting(const timers::Trigger& trigger) {
    if (trigger.key) return "attend la touche de timer " + std::to_string(trigger.keyIndex);
    if (trigger.requireCombat) return "attend le combat dans la zone";
    if (trigger.requireOutOfCombat) return "attend la sortie de combat";
    if (trigger.requireDeparture) return "attend la sortie de la zone";
    return "attend l'entrée dans la zone";
}

bool VoiceCombo(const char* label, std::string& id, const std::vector<VoiceInfo>& voices, const char* fallback) {
    std::string preview = fallback;
    for (const auto& voice : voices) if (voice.id == id) preview = voice.name;
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str())) {
        if (ImGui::Selectable(fallback, id.empty())) { id.clear(); changed = true; }
        for (const auto& voice : voices) {
            std::string text = voice.name + (voice.language.empty() ? "" : " · " + voice.language) + "##" + voice.id;
            if (ImGui::Selectable(text.c_str(), voice.id == id)) { id = voice.id; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}
} // namespace

void AlertsOnArcEvent(void* payload) { AurasOnEvent(payload); }

void AlertsLoad(AddonAPI_t* api, NexusLinkData_t* nexus, Mumble::Data* mumble, const std::string& settingsPath) {
    Api = api;
    Nexus = nexus;
    Link = mumble;
    SettingsFile = settingsPath;
    std::error_code error;
    ForgeDir = (fs::path(api->Paths_GetAddonDirectory("Forge")) / "timers").string();
    fs::create_directories(ForgeDir, error);
    // The player's own .wav files join the sound menus.
    SoundsDir = (fs::path(api->Paths_GetAddonDirectory("Forge")) / "sounds").string();
    fs::create_directories(SoundsDir, error);
    SetSoundFolder(SoundsDir);
    // WeakAuras' sounds, once: they come with the addon's repository, not inside the DLL.
    if (!fs::exists(fs::path(SoundsDir) / "WeakAuras" / "AirHorn.ogg", error)) InstallWeakAurasSounds();
    // TaimiHUD keeps its packs under addons/Taimi/timers (older builds: TaimiHUD). Built from the addons folder: no
    // folder gets created for an addon the player does not have.
    fs::path addons(api->Paths_GetAddonDirectory(nullptr));
    TaimiDirs = { (addons / "Taimi" / "timers").string(), (addons / "TaimiHUD" / "timers").string() };
    for (int key = 0; key < KEY_COUNT; key++) {
        api->InputBinds_RegisterWithString(KEY_IDS[key], OnKey, "(null)");
        api->Localization_Set(KEY_IDS[key], "fr", ("Forge · Alertes : touche de timer " + std::to_string(key)).c_str());
        api->Localization_Set(KEY_IDS[key], "en", ("Forge · Alerts: timer trigger key " + std::to_string(key)).c_str());
    }
    api->InputBinds_RegisterWithString(KB_RESET, OnKey, "(null)");
    api->Localization_Set(KB_RESET, "fr", "Forge · Alertes : réarmer les timers");
    api->Localization_Set(KB_RESET, "en", "Forge · Alerts: re-arm the timers");
    SpeechStart();
    bool withTaimi;
    { std::lock_guard<std::mutex> lock(g_state.mutex); withTaimi = g_state.settings.timersFromTaimi; }
    ReadTimers(withTaimi);
}

void AlertsUnload() {
    for (int key = 0; key < KEY_COUNT; key++) Api->InputBinds_Deregister(KEY_IDS[key]);
    Api->InputBinds_Deregister(KB_RESET);
    SpeechStop();
    if (Loader.joinable()) Loader.join();
    if (Downloader.joinable()) Downloader.join();
    if (SoundsDownloader.joinable()) SoundsDownloader.join();
    Run.machines.clear();
}

void AlertsRender() {
    Settings st;
    { std::lock_guard<std::mutex> lock(g_state.mutex); st = g_state.settings; }
    if (ReloadRequested.exchange(false)) ReadTimers(st.timersFromTaimi);
    double now = Now();
    std::vector<timers::Bar> bars;
    std::vector<timers::Shout> shouts;
    TickTimers(st, now, bars, shouts);
    bool demo = now < DemoUntil;
    static double lastFrame = 0;
    if (demo) {
        DemoBars(now, bars);
        DemoShouts(lastFrame, now, shouts);
    }
    lastFrame = now;
    std::set<std::string> said;
    // Windows plays one sound at a time: this frame's most important one goes (a moment, then an alert, then a tick).
    std::string tone;
    int toneRank = 0;
    auto sound = [&](const std::string& id, int rank) { if (!id.empty() && rank > toneRank) { tone = id; toneRank = rank; } };
    for (const auto& shout : shouts) {
        switch (shout.kind) {
        case timers::Shout::Sound:
            if (st.speakSounds && said.insert(shout.text).second) SayTimer(st, shout.text);
            if (st.centerText) Flashes.push_back({ shout.text, IM_COL32(255, 255, 255, 255), now + shout.duration });
            break;
        case timers::Shout::Alert:
            if (st.speakAlerts && said.insert(shout.text).second) SayTimer(st, shout.text);
            sound(st.alertSound, 2);
            break;
        case timers::Shout::Warning:
            if (st.speakWarnings && said.insert(shout.text).second) SayTimer(st, shout.text);
            break;
        case timers::Shout::Tick:
            if (CountdownSecond(st, shout.seconds)) sound(WarningSoundFor(st, shout.timer, shout.text, false), 1);
            break;
        case timers::Shout::Due:
            sound(WarningSoundFor(st, shout.timer, shout.text, true), 3);
            break;
        case timers::Shout::Reset:
            Toasts.push_back({ "Timer réarmé : " + shout.text, IM_COL32(200, 200, 200, 255), now + shout.duration });
            break;
        }
    }
    if (!tone.empty()) PlayTone(tone, st.soundVolume);
    Flashes.erase(std::remove_if(Flashes.begin(), Flashes.end(), [&](const Flash& flash) { return flash.until <= now; }), Flashes.end());
    Toasts.erase(std::remove_if(Toasts.begin(), Toasts.end(), [&](const Flash& toast) { return toast.until <= now; }), Toasts.end());
    // Nothing over loading screens, cutscenes or the map.
    if (!demo && ((Nexus && !Nexus->IsGameplay) || (Link && Link->Context.IsMapOpen))) return;
    if (Nexus && Nexus->Font) ImGui::PushFont((ImFont*)Nexus->Font);
    RenderBars(st, bars, now);
    RenderCentre(st, bars, now);
    RenderAuras(st, now, demo);
    if (Nexus && Nexus->Font) ImGui::PopFont();
}

void AlertsTab() {
    Settings& st = g_state.settings;
    bool changed = false, machinesChanged = false;
    double now = Now();
    auto lib = CurrentLibrary();
    ImGui::TextColored(GOLD, "%s", "Timers de combat");
    ImGui::SameLine();
    ImGui::TextColored(MUTED, "· format Blish HUD / TaimiHUD (.bhtimer), comme Hero's Timers");
    if (lib->loaded) ImGui::TextColored(MUTED, "%d timers chargés (Forge : %d, TaimiHUD : %d) · carte %u : %d ici", (int)lib->files.size(), lib->fromForge, lib->fromTaimi, Run.map, (int)Run.machines.size());
    else ImGui::TextColored(MUTED, "%s", "Lecture des timers…");
    if (lib->loaded && lib->files.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(GOLD, "%s", "Aucun timer trouvé. Hero's Timers (QuitarHero) couvre les raids, strikes et donjons : champignons de Slothasor, verts de Dhuum, huiles de Deimos…");
        ImGui::PopTextWrapPos();
    }
    if (Downloading) ImGui::TextColored(MUTED, "%s", GetDownloadStatus().c_str());
    else {
        if (ImGui::Button(lib->fromForge ? "Mettre à jour Hero's Timers" : "Télécharger Hero's Timers")) DownloadHero();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Dernière version de github.com/QuitarHero/Hero-Timers, dans addons\\Forge\\timers\\Hero-Timers.");
        ImGui::SameLine();
        if (ImGui::Button("Recharger")) ReloadRequested = true;
        ImGui::SameLine();
        if (ImGui::Button("Ouvrir le dossier")) ShellExecuteW(nullptr, L"open", Wide(ForgeDir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        std::string status = GetDownloadStatus();
        if (!status.empty()) ImGui::TextColored(MUTED, "%s", status.c_str());
    }

    ImGui::Separator();
    if (Run.machines.empty()) ImGui::TextColored(MUTED, "%s", st.timersOn ? "Aucun timer pour cette carte." : "Timers désactivés.");
    for (const auto& machine : Run.machines) {
        const timers::TimerFile& file = machine->File();
        ImGui::TextUnformatted(file.Title().c_str());
        ImGui::SameLine();
        switch (machine->GetState()) {
        case timers::Machine::State::OnMap: ImGui::TextColored(MUTED, "· %s", Waiting(file.phases[0].start).c_str()); break;
        case timers::Machine::State::OnPhase: ImGui::TextColored(GREEN, "· « %s » %s", file.phases[machine->PhaseIndex()].name.c_str(), Clock(machine->Elapsed(now)).c_str()); break;
        case timers::Machine::State::BetweenPhases: {
            int next = machine->PhaseIndex() + 1;
            ImGui::TextColored(MUTED, "· entre deux phases, %s", next < (int)file.phases.size() ? Waiting(file.phases[next].start).c_str() : "");
            break;
        }
        case timers::Machine::State::Finished: ImGui::TextColored(MUTED, "%s", "· terminé, se réarme hors combat"); break;
        }
    }
    if (ImGui::Button("Réarmer les timers")) ResetRequested = true;
    ImGui::SameLine();
    ImGui::TextColored(MUTED, "%s", "Touches :");
    for (int key = 0; key < KEY_COUNT; key++) {
        ImGui::SameLine();
        if (ImGui::SmallButton(std::to_string(key).c_str())) { KeysDown.fetch_or(1u << key); KeysUp.fetch_or(1u << key); }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Les « Trigger Key 0 à 4 » des timers. À assigner dans Nexus → Raccourcis : Forge · Alertes : touche de timer 0 à 4.");
    if (ImGui::Button("Tester l'affichage, la voix et les sons")) {
        DemoStart = now;
        DemoUntil = now + 12.0;
        SayTimer(st, "Move!");
        SayAura(st, "Célérité");
    }
    ImGui::SameLine();
    bool placing = !st.alertsLocked;
    if (ImGui::Checkbox("Déplacer les barres et les auras", &placing)) { st.alertsLocked = !placing; changed = true; }

    if (ImGui::CollapsingHeader("Affichage, voix et sons")) {
        if (ImGui::Checkbox("Timers de combat", &st.timersOn)) { changed = true; machinesChanged = true; }
        ImGui::SameLine();
        if (ImGui::Checkbox("Lire aussi les timers de TaimiHUD", &st.timersFromTaimi)) { changed = true; ReloadRequested = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "addons\\Taimi\\timers. Si TaimiHUD affiche déjà ses timers, désactive-les d'un côté pour ne pas tout voir deux fois.");
        changed |= ImGui::Checkbox("Texte au centre de l'écran", &st.centerText);
        changed |= ImGui::SliderFloat("Taille du texte central", &st.centerScale, 0.8f, 3.0f, "%.1f");
        changed |= ImGui::SliderFloat("Hauteur du texte central", &st.centerY, 0.05f, 0.8f, "%.2f");
        changed |= ImGui::SliderFloat("Largeur des barres", &st.barWidth, 180.0f, 520.0f, "%.0f");
        changed |= ImGui::SliderInt("Voix : avertissement lu à", &st.warnAt, 0, 10, "%d s");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Un avertissement est lu, et compté au centre, ce nombre de secondes avant son moment. 0 : jamais.");
        changed |= ImGui::Checkbox("Lire les annonces des timers", &st.speakSounds);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Lire les alertes", &st.speakAlerts);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Lire les avertissements", &st.speakWarnings);
        ImGui::Spacing();
        ImGui::TextColored(GOLD, "%s", "Sons avant le moment, comme WeakAuras");
        std::vector<std::string> files = SoundFiles();
        changed |= SoundCombo("Compte à rebours", st.countdownSound, files, false, st.soundVolume);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Joué à chacune des dernières secondes d'un avertissement : wuh, wuh, wuh… puis le son du moment.");
        changed |= ImGui::SliderInt("Dernières secondes", &st.countdownFrom, 0, 10, "%d s");
        changed |= ImGui::SliderInt("Un son toutes les", &st.countdownEvery, 1, 3, "%d s");
        changed |= SoundCombo("Au moment", st.dueSound, files, false, st.soundVolume);
        changed |= SoundCombo("Quand une alerte s'affiche", st.alertSound, files, false, st.soundVolume);
        changed |= ImGui::SliderInt("Volume des sons", &st.soundVolume, 0, 100);
        int weakAuras = (int)std::count_if(files.begin(), files.end(), FromWeakAuras);
        if (SoundsDownloading) ImGui::TextColored(MUTED, "%s", GetSoundsStatus().c_str());
        else {
            ImGui::TextColored(MUTED, "Sons WeakAuras : %d installés", weakAuras);
            ImGui::SameLine();
            if (ImGui::SmallButton(weakAuras ? "Réinstaller" : "Installer")) InstallWeakAurasSounds();
            std::string status = GetSoundsStatus();
            if (!status.empty() && !weakAuras) ImGui::TextColored(GOLD, "%s", status.c_str());
        }
        if (ImGui::SmallButton("Dossier des sons")) ShellExecuteW(nullptr, L"open", Wide(SoundsDir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Tes propres sons .ogg ou .wav, posés dans addons\\Forge\\sounds, s'ajoutent aux menus.");
        ImGui::SameLine();
        ImGui::TextColored(MUTED, "%s", "Chaque mécanique peut avoir ses sons : Timers chargés, bouton Sons du timer.");
        ImGui::Spacing();
        std::vector<VoiceInfo> voices = SpeechVoices();
        if (voices.empty()) ImGui::TextColored(MUTED, "%s", "Aucune voix Windows trouvée pour l'instant.");
        changed |= VoiceCombo("Voix des timers", st.timerVoice, voices, "Voix anglaise (auto)");
        changed |= VoiceCombo("Voix des auras", st.auraVoice, voices, "Voix de Windows");
        changed |= ImGui::SliderInt("Volume", &st.voiceVolume, 0, 100);
        changed |= ImGui::SliderInt("Débit", &st.voiceRate, -5, 8);
        if (ImGui::SmallButton("Écouter")) { SayTimer(st, "Next: Mushroom number 2"); SayAura(st, "Stabilité"); }
    }

    if (ImGui::CollapsingHeader("Auras : tes avantages")) {
        changed |= ImGui::Checkbox("Auras", &st.aurasOn);
        ImGui::SameLine();
        changed |= ImGui::SliderFloat("Taille des icônes", &st.auraSize, 24.0f, 96.0f, "%.0f");
        if (!AurasEverBuff()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(MUTED, "%s", "Aucun avantage reçu d'arcdps pour l'instant : les auras s'animent dès qu'arcdps signale tes avantages (arcdps à jour, intégration Nexus active).");
            ImGui::PopTextWrapPos();
        } else {
            AuraCounts counts = AurasCounts();
            ImGui::TextColored(MUTED, "arcdps : %u événements sur tes avantages, %u retraits de piles déjà expirées ignorés", counts.events, counts.unknownRemoves);
        }
        if (ImGui::BeginTable("forge-auras", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Avantage", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Afficher", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Minimum", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("En combat", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Si absente", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableHeadersRow();
            for (auto& rule : st.auras) {
                const BoonInfo* boon = FindBoon(rule.buff);
                if (!boon) continue;
                std::string id = std::to_string(rule.buff);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                changed |= ImGui::Checkbox(("##on" + id).c_str(), &rule.on);
                ImGui::SameLine();
                if (Texture_t* icon = BoonTexture(boon); icon && icon->Resource) { ImGui::Image((ImTextureID)icon->Resource, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize())); ImGui::SameLine(); }
                ImGui::TextUnformatted(boon->name);
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1);
                changed |= ImGui::Combo(("##show" + id).c_str(), &rule.show, "Toujours\0Présente\0Absente\0");
                ImGui::TableSetColumnIndex(2);
                if (boon->intensity) { ImGui::SetNextItemWidth(-1); changed |= ImGui::SliderInt(("##min" + id).c_str(), &rule.minStacks, 1, 25, "%d stacks"); }
                else ImGui::TextColored(MUTED, "%s", "—");
                ImGui::TableSetColumnIndex(3);
                changed |= ImGui::Checkbox(("##combat" + id).c_str(), &rule.combatOnly);
                ImGui::TableSetColumnIndex(4);
                ImGui::SetNextItemWidth(-1);
                changed |= ImGui::Combo(("##sound" + id).c_str(), &rule.sound, "Rien\0Voix\0Bip\0");
            }
            ImGui::EndTable();
        }
        ImGui::TextColored(MUTED, "%s", "« En combat » : l'absence ne compte (cadre rouge, son) qu'en combat.");
    }

    if (ImGui::CollapsingHeader(("Timers chargés (" + std::to_string(lib->files.size()) + ")###forge-timers").c_str())) {
        std::map<std::string, std::vector<std::shared_ptr<const timers::TimerFile>>> byCategory;
        for (const auto& file : lib->files) byCategory[file->category.empty() ? "Autres" : file->category].push_back(file);
        for (const auto& [category, files] : byCategory) {
            if (!ImGui::TreeNode((category + " (" + std::to_string(files.size()) + ")###" + category).c_str())) continue;
            for (const auto& file : files) {
                bool on = Enabled(st, *file);
                std::string label = file->Area() + " · " + file->Title() + "##" + file->id;
                if (ImGui::Checkbox(label.c_str(), &on)) {
                    std::vector<std::string>& list = OffByDefault(*file) ? st.timersOptIn : st.timersOff;
                    bool listed = OffByDefault(*file) ? on : !on;
                    list.erase(std::remove(list.begin(), list.end(), file->id), list.end());
                    if (listed) list.push_back(file->id);
                    changed = true;
                    machinesChanged = true;
                }
                if (ImGui::IsItemHovered()) {
                    std::string keys;
                    for (int key : file->keys) keys += (keys.empty() ? "" : ", ") + std::to_string(key);
                    std::string tip = file->description + "\n\n" + file->source + (keys.empty() ? "" : " · touches " + keys) + " · carte " + std::to_string(file->map) + (OffByDefault(*file) ? "\nEntraînement hors combat : désactivé tant que tu ne le coches pas." : "");
                    ImGui::SetTooltip("%s", tip.c_str());
                }
                std::vector<std::string> warnings = WarningsOf(*file);
                if (warnings.empty()) continue;
                ImGui::SameLine();
                std::string popup = "forge-sons-" + file->id;
                if (ImGui::SmallButton(("Sons##" + file->id).c_str())) ImGui::OpenPopup(popup.c_str());
                if (ImGui::BeginPopup(popup.c_str())) {
                    // Each mechanic's own countdown and moment sounds, like an aura of its own in WeakAuras.
                    std::vector<std::string> files = SoundFiles();
                    ImGui::TextColored(GOLD, "%s", file->Title().c_str());
                    ImGui::TextColored(MUTED, "Par défaut : %s sur les %d dernières secondes, %s au moment.", SoundName(st.countdownSound).c_str(), st.countdownFrom, SoundName(st.dueSound).c_str());
                    if (ImGui::BeginTable("sons", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("Avertissement");
                        ImGui::TableSetupColumn("Compte à rebours");
                        ImGui::TableSetupColumn("Au moment");
                        ImGui::TableHeadersRow();
                        for (const auto& text : warnings) {
                            WarningSound& own = st.warningSounds[file->id + "\n" + text];
                            ImGui::PushID(text.c_str());
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(text.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::SetNextItemWidth(160);
                            changed |= SoundCombo("##compte", own.countdown, files, true, st.soundVolume);
                            ImGui::TableSetColumnIndex(2);
                            ImGui::SetNextItemWidth(160);
                            changed |= SoundCombo("##moment", own.due, files, true, st.soundVolume);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::TreePop();
        }
        if (!lib->errors.empty() && ImGui::TreeNode(("Fichiers ignorés (" + std::to_string(lib->errors.size()) + ")").c_str())) {
            for (const auto& error : lib->errors) ImGui::TextColored(MUTED, "%s", error.c_str());
            ImGui::TreePop();
        }
    }
    if (machinesChanged) SettingsVersion++;
    if (changed) SaveSettingsLocked(SettingsFile);
}
