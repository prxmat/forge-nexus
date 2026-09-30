// Is arcdps there, current and alive? Every log Forge sees comes from it, and some game updates break it
// until deltaconnected ships a new build. This tells the player instead of leaving an empty window.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>

enum class ArcVerdict { Unknown, Absent, Outdated, GameUpdated, Silent, Offline, Ok };

struct ArcdpsState {
    std::mutex mutex;
    bool checked = false;
    bool loaded = false;        // A module of the game exports arcdps' extension API.
    std::wstring modulePath;
    std::string localMd5, remoteMd5, remoteVersion;
    bool remoteKnown = false;   // deltaconnected answered.
    bool gameUpdated = false;   // The game's build changed since the last session.
    uint64_t lastEventMs = 0;   // Last combat event arcdps sent through Nexus.
    uint64_t combatSinceMs = 0; // When the current combat began, 0 out of combat.
};

extern ArcdpsState g_arcdps;

// Finds the module, hashes it and compares with the build deltaconnected publishes. Blocking: network.
void ArcdpsCheck();
// A combat event came through Nexus.
void ArcdpsNoteEvent();
// Mumble's combat flag, each frame.
void ArcdpsNoteCombat(bool inCombat);
// Opens arcdps' download page in the browser: the player installs it, as always.
void ArcdpsOpenDownload();
ArcVerdict ArcdpsVerdict();
// A sentence for the player, for the verdict.
std::string ArcdpsText(ArcVerdict verdict);
// "ok", "outdated"… for Forge.
const char* ArcdpsCode(ArcVerdict verdict);
