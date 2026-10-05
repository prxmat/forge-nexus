#include "fights.h"
#include "live.h"
#include "forge.h"

#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>

FightsState g_fights;

namespace {
constexpr size_t HISTORY = 30;
constexpr int SCAN_MS = 2000;
std::atomic<bool> running{ false };
std::thread watcher;

std::wstring DocumentsDir() {
    wchar_t* path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)) && path) out = path;
    if (path) CoTaskMemFree(path);
    return out;
}

bool EndsWith(const std::wstring& text, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return text.size() >= n && _wcsicmp(text.c_str() + text.size() - n, suffix) == 0;
}

// Log files under the folder (arcdps makes a folder per boss and per character), newer than `since`.
void Walk(const std::wstring& dir, const FILETIME& since, std::unordered_map<std::wstring, uint64_t>& found, int depth = 0) {
    if (depth > 6) return;
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (data.cFileName[0] == L'.') continue;
        std::wstring path = dir + L"\\" + data.cFileName;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { Walk(path, since, found, depth + 1); continue; }
        if (!EndsWith(path, L".zevtc") && !EndsWith(path, L".evtc")) continue;
        if (CompareFileTime(&data.ftLastWriteTime, &since) < 0) continue;
        found[path] = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
}

void Loop() {
    std::unordered_set<std::wstring> seen;
    std::unordered_map<std::wstring, std::pair<uint64_t, int>> settling; // size, stable ticks
    FILETIME since{};
    {
        // Only logs written after the addon loaded (minus a minute).
        SYSTEMTIME now{};
        GetSystemTime(&now);
        SystemTimeToFileTime(&now, &since);
        ULARGE_INTEGER value{ since.dwLowDateTime, since.dwHighDateTime };
        value.QuadPart -= 60ULL * 10000000ULL;
        since.dwLowDateTime = value.LowPart;
        since.dwHighDateTime = value.HighPart;
    }
    while (running) {
        std::wstring dir;
        { std::lock_guard<std::mutex> lock(g_fights.mutex); dir = g_fights.logsDir; }
        if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::lock_guard<std::mutex> lock(g_fights.mutex);
            g_fights.status = "Dossier des logs introuvable (Options > Forge).";
        } else {
            std::unordered_map<std::wstring, uint64_t> found;
            Walk(dir, since, found);
            for (const auto& [path, size] : found) {
                if (seen.count(path)) continue;
                auto& state = settling[path];
                if (state.first != size) { state = { size, 0 }; continue; }
                if (++state.second < 2) continue; // Two scans at the same size: arcdps is done writing.
                // A .evtc next to its .zevtc: arcdps compresses and removes the raw file, keep the zip.
                if (EndsWith(path, L".evtc") && found.count(path.substr(0, path.size() - 5) + L".zevtc")) { seen.insert(path); continue; }
                seen.insert(path);
                settling.erase(path);
                ParsedFight fight = ParseEvtcFile(path);
                size_t slash = path.find_last_of(L'\\');
                std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
                fight.file = std::string(base.begin(), base.end());
                {
                    std::lock_guard<std::mutex> lock(g_fights.mutex);
                    if (fight.ok) {
                        LiveDismiss();
                        g_fights.fights.push_front(fight);
                        if (g_fights.fights.size() > HISTORY) g_fights.fights.pop_back();
                        g_fights.selected = 0;
                        g_fights.status = "Dernier log lu : " + fight.file;
                    } else {
                        g_fights.status = "Log illisible (" + fight.error + ") : " + fight.file;
                    }
                }
                // Outside the lock: the report is a network call.
                if (!fight.ok) ReportError("Log illisible : " + fight.error, "{\"file\":\"" + fight.file + "\"}");
                else if (fight.wvw && fight.teams.empty() && fight.eventsRead) ReportError("Log McM sans équipe", "{\"file\":\"" + fight.file + "\",\"agents\":" + std::to_string(fight.agentsRead) + ",\"players\":" + std::to_string(fight.playersRead) + ",\"withTeam\":" + std::to_string(fight.playersWithTeam) + ",\"colours\":" + std::to_string(fight.coloursKnown) + "}");
            }
            if (g_fights.status.empty()) { std::lock_guard<std::mutex> lock(g_fights.mutex); g_fights.status = "En attente du prochain combat..."; }
        }
        for (int i = 0; i < SCAN_MS / 100 && running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
} // namespace

std::wstring DefaultLogsDir() {
    std::wstring documents = DocumentsDir();
    return documents.empty() ? L"" : documents + L"\\Guild Wars 2\\addons\\arcdps\\arcdps.cbtlogs";
}

void StartFightsWatcher() {
    running = true;
    watcher = std::thread(Loop);
}

void StopFightsWatcher() {
    running = false;
    if (watcher.joinable()) watcher.join();
}
