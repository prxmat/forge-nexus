#include "arcdps.h"

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <algorithm>
#include <vector>
#include "forge.h"

ArcdpsState g_arcdps;

namespace {
std::vector<char> ReadWholeFile(const std::wstring& path) {
    std::vector<char> bytes;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return bytes;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64LL * 1024 * 1024) {
        bytes.resize((size_t)size.QuadPart);
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), (DWORD)bytes.size(), &read, nullptr) || read != bytes.size()) bytes.clear();
    }
    CloseHandle(file);
    return bytes;
}

const char* ARC_BASE = "https://www.deltaconnected.com/arcdps/x64/";
constexpr uint64_t SILENT_AFTER_MS = 30000;

std::string Md5(const char* data, size_t size) {
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    std::string out;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return out;
    if (CryptCreateHash(provider, CALG_MD5, 0, 0, &hash)) {
        if (CryptHashData(hash, (const BYTE*)data, (DWORD)size, 0)) {
            BYTE digest[16];
            DWORD length = sizeof(digest);
            if (CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0)) {
                char hex[33];
                for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
                out = hex;
            }
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(provider, 0);
    return out;
}

std::string Trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

// arcdps is d3d11.dll in the game folder, or gw2addon_arcdps.dll under an addon loader: whichever it is, it
// is the one module of the process that exports its extension API.
std::wstring FindArcdps() {
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return L"";
    size_t count = std::min<size_t>(needed / sizeof(HMODULE), 1024);
    for (size_t i = 0; i < count; i++) {
        if (!GetProcAddress(modules[i], "addextension2") || !GetProcAddress(modules[i], "e3")) continue;
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(modules[i], path, MAX_PATH)) return path;
    }
    return L"";
}
} // namespace

void ArcdpsCheck() {
    std::wstring path = FindArcdps();
    std::string localMd5;
    if (!path.empty()) {
        std::vector<char> bytes = ReadWholeFile(path);
        localMd5 = Md5(bytes.data(), bytes.size());
    }
    // deltaconnected publishes the md5 and the version of the current build next to the dll.
    std::string sum, version;
    bool remote = HttpGet(std::string(ARC_BASE) + "d3d11.dll.md5sum", "", sum) == 200 && sum.size() >= 32
        && HttpGet(std::string(ARC_BASE) + "d3d11.dll.version", "", version) == 200 && !version.empty();
    std::lock_guard<std::mutex> lock(g_arcdps.mutex);
    g_arcdps.checked = true;
    g_arcdps.loaded = !path.empty();
    g_arcdps.modulePath = path;
    g_arcdps.localMd5 = localMd5;
    if (remote) {
        g_arcdps.remoteKnown = true;
        g_arcdps.remoteMd5 = sum.substr(0, 32);
        g_arcdps.remoteVersion = Trim(version);
    }
}

void ArcdpsNoteEvent() {
    std::lock_guard<std::mutex> lock(g_arcdps.mutex);
    g_arcdps.lastEventMs = GetTickCount64();
}

void ArcdpsNoteCombat(bool inCombat) {
    std::lock_guard<std::mutex> lock(g_arcdps.mutex);
    if (!inCombat) g_arcdps.combatSinceMs = 0;
    else if (!g_arcdps.combatSinceMs) g_arcdps.combatSinceMs = GetTickCount64();
}

void ArcdpsOpenDownload() {
    ShellExecuteW(nullptr, L"open", L"https://www.deltaconnected.com/arcdps/", nullptr, nullptr, SW_SHOWNORMAL);
}

ArcVerdict ArcdpsVerdict() {
    std::lock_guard<std::mutex> lock(g_arcdps.mutex);
    const ArcdpsState& a = g_arcdps;
    if (!a.checked) return ArcVerdict::Unknown;
    if (!a.loaded) return ArcVerdict::Absent;
    if (a.remoteKnown && a.localMd5 != a.remoteMd5) return a.gameUpdated ? ArcVerdict::GameUpdated : ArcVerdict::Outdated;
    if (a.combatSinceMs && GetTickCount64() - a.combatSinceMs > SILENT_AFTER_MS && a.lastEventMs < a.combatSinceMs) return ArcVerdict::Silent;
    if (!a.remoteKnown) return ArcVerdict::Offline;
    return ArcVerdict::Ok;
}

std::string ArcdpsText(ArcVerdict verdict) {
    std::string version;
    { std::lock_guard<std::mutex> lock(g_arcdps.mutex); version = g_arcdps.remoteVersion; }
    switch (verdict) {
    case ArcVerdict::Unknown: return "Vérification d'arcdps…";
    case ArcVerdict::Absent: return "arcdps introuvable dans le jeu : aucun log ne sera écrit, ni pour l'addon ni pour Forge.";
    case ArcVerdict::Outdated: return "Mise à jour arcdps disponible (" + version + "). Sans elle, les logs peuvent s'arrêter à la prochaine mise à jour du jeu.";
    case ArcVerdict::GameUpdated: return "Le jeu a été mis à jour, arcdps pas encore chez toi (" + version + " disponible) : les logs peuvent manquer cette sortie.";
    case ArcVerdict::Silent: return "En combat depuis 30 s sans un événement arcdps : il est peut-être cassé par la dernière mise à jour du jeu.";
    case ArcVerdict::Offline: return "arcdps chargé, version non vérifiée (deltaconnected injoignable).";
    case ArcVerdict::Ok: return "arcdps " + version + " à jour.";
    }
    return "";
}

const char* ArcdpsCode(ArcVerdict verdict) {
    switch (verdict) {
    case ArcVerdict::Unknown: return "unknown";
    case ArcVerdict::Absent: return "absent";
    case ArcVerdict::Outdated: return "outdated";
    case ArcVerdict::GameUpdated: return "game-updated";
    case ArcVerdict::Silent: return "silent";
    case ArcVerdict::Offline: return "offline";
    case ArcVerdict::Ok: return "ok";
    }
    return "unknown";
}
