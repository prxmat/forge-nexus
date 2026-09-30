// Watches the arcdps log folder and keeps the fights of the session, newest first, for the window.
#pragma once
#include <deque>
#include <mutex>
#include <string>
#include "evtc.h"

struct FightsState {
    std::mutex mutex;
    std::deque<ParsedFight> fights; // Newest first, HISTORY at most.
    int selected = 0;               // Index in `fights` shown in detail.
    std::wstring logsDir;
    std::string status;             // What the watcher is doing or missing.
    bool squadOnly = false;         // WvW: the recorder's squad rather than the whole team.
};

extern FightsState g_fights;

std::wstring DefaultLogsDir();
void StartFightsWatcher();
void StopFightsWatcher();
